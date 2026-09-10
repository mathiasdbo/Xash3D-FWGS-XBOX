// XB census #1 (docs/r3d/fork-plan.md, "Two censuses first"): how many
// faces per map carry a second/third/fourth lightstyle (styles[1..3] !=
// 255 - GoldSrc's marker for "this slot unused", common/bspfile.h's
// LM_STYLES = 4). BspParser.cpp bakes styles[0] only and drops the rest
// (see its own comment at the hasLightmap computation) - "RGB565 pages,
// no runtime conversion" currently means no light in the game switches
// or flickers on Xbox. This number is the fork-plan's own input to
// choosing between per-style pages combined at runtime, a baked "on"
// state with binary switching, or an accepted loss - not a design this
// tool makes, only the measurement the design is supposed to be made
// from.
//
// Usage: census_lightstyles <maps-dir> [<maps-dir> ...]
// Scans every *.bsp directly inside each given directory (non-recursive -
// GoldSrc installs keep maps flat in valve/maps/, no subdirectories).
#include "formats/BspParser.h"
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

using namespace formats;
namespace fs = std::filesystem;

namespace {

struct MapResult {
    std::string name;
    size_t numFaces = 0;
    size_t facesWithLightmap = 0; // styles[0] != 255 (BspParser's own hasLightmap gate, minus TEX_SPECIAL - not checked here, texinfo/flags aren't needed for this count)
    size_t facesWithExtraStyle = 0; // styles[1..3] contains at least one != 255
    size_t extraStyleSlotsTotal = 0; // sum over such faces of how many of styles[1..3] are used - a face can carry up to 3
};

bool censusOneMap(const fs::path& bspPath, MapResult& out) {
    BspParser parser;
    if (!parser.load(bspPath.string())) {
        std::fprintf(stderr, "  SKIP (failed to load): %s\n", bspPath.string().c_str());
        return false;
    }

    const auto& faceData = parser.getLumpData(LUMP_FACES);
    if (faceData.size() % sizeof(dface_t) != 0) {
        std::fprintf(stderr, "  SKIP (LUMP_FACES size %zu not a multiple of dface_t): %s\n",
                     faceData.size(), bspPath.string().c_str());
        return false;
    }

    const dface_t* faces = reinterpret_cast<const dface_t*>(faceData.data());
    size_t numFaces = faceData.size() / sizeof(dface_t);

    out.name = bspPath.stem().string();
    out.numFaces = numFaces;

    for (size_t i = 0; i < numFaces; ++i) {
        const dface_t& f = faces[i];
        if (f.styles[0] == 255)
            continue; // BspParser's own hasLightmap gate starts here; no lightmap at all
        out.facesWithLightmap++;

        int extra = 0;
        for (int s = 1; s < 4; ++s) {
            if (f.styles[s] != 255)
                ++extra;
        }
        if (extra > 0) {
            out.facesWithExtraStyle++;
            out.extraStyleSlotsTotal += static_cast<size_t>(extra);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <maps-dir> [<maps-dir> ...]\n", argv[0]);
        return 1;
    }

    std::vector<MapResult> results;
    size_t scanned = 0, skipped = 0;

    for (int a = 1; a < argc; ++a) {
        fs::path dir(argv[a]);
        if (!fs::is_directory(dir)) {
            std::fprintf(stderr, "Not a directory, skipping: %s\n", argv[a]);
            continue;
        }
        std::vector<fs::path> bsps;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bsp")
                bsps.push_back(entry.path());
        }
        std::sort(bsps.begin(), bsps.end());

        for (const auto& bsp : bsps) {
            MapResult r;
            if (censusOneMap(bsp, r)) {
                results.push_back(r);
                ++scanned;
            } else {
                ++skipped;
            }
        }
    }

    std::sort(results.begin(), results.end(), [](const MapResult& a, const MapResult& b) {
        return a.facesWithExtraStyle > b.facesWithExtraStyle;
    });

    std::printf("%-24s %8s %8s %8s %8s\n", "map", "faces", "lit", "multi", "slots");
    size_t totalFaces = 0, totalLit = 0, totalMulti = 0, totalSlots = 0;
    size_t mapsWithAny = 0;
    for (const auto& r : results) {
        std::printf("%-24s %8zu %8zu %8zu %8zu\n", r.name.c_str(), r.numFaces,
                    r.facesWithLightmap, r.facesWithExtraStyle, r.extraStyleSlotsTotal);
        totalFaces += r.numFaces;
        totalLit += r.facesWithLightmap;
        totalMulti += r.facesWithExtraStyle;
        totalSlots += r.extraStyleSlotsTotal;
        if (r.facesWithExtraStyle > 0)
            ++mapsWithAny;
    }

    std::printf("\n%zu map(s) scanned, %zu skipped (load/format failures)\n", scanned, skipped);
    std::printf("%zu map(s) have at least one multi-style face\n", mapsWithAny);
    std::printf("totals: %zu faces, %zu lit, %zu with an extra style, %zu extra-style slot(s) total\n",
                totalFaces, totalLit, totalMulti, totalSlots);
    if (totalLit > 0) {
        std::printf("%.2f%% of lit faces carry at least one extra lightstyle\n",
                    100.0 * static_cast<double>(totalMulti) / static_cast<double>(totalLit));
    }

    return 0;
}
