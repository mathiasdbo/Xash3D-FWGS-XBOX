// XB census #2 (docs/r3d/fork-plan.md, "Two censuses first"): which
// retail models reference more bones per submodel than a VP20 vertex
// shader palette holds - 192 constants total
// (deps/nxdk/tools/vp20compiler/config.h), three per bone, roughly 50
// left after the view-projection and light constants a shader also
// needs, against MAXSTUDIOBONES 128 (deps/hlsdk/engine/studio.h). Every
// model over the line needs partitioning by bone subset before it can
// be rendered on the NV2A; the census is what says which ones, not a
// design this tool makes.
//
// GoldSrc studio models rig each vertex to exactly ONE bone (not a
// weighted multi-bone skin): mstudiomodel_t::vertinfoindex points to
// numverts bytes, one per vertex, each the index of the bone it's rigid
// to (deps/hlsdk/engine/studio.h's own mstudiomodel_t comment: "vertex
// bone info"). "Bones per submodel" here means the count of DISTINCT
// bytes in that array for one mstudiomodel_t - the real per-draw bone
// set a partitioning pass would need to cover, not studiohdr_t's own
// numbones (the whole skeleton, most of which the geometry in any one
// submodel that a body-part choice selects rarely touches).
//
// Usage: census_bones <models-dir> [<models-dir> ...]
// Recurses (GoldSrc installs keep models/player/<char>/, models/weapons/
// etc. as real subdirectories, unlike maps/).
#include "formats/MdlParser.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

using namespace formats;
namespace fs = std::filesystem;

namespace {

// Not in MdlParser.h (it only models texture/skin data) - defined here from
// deps/hlsdk/engine/studio.h verbatim (mstudiobodyparts_t :253-259,
// mstudiomodel_t :277-299), not guessed.
#pragma pack(push, 1)
struct mstudiobodyparts_t {
    char name[64];
    int32_t nummodels;
    int32_t base;
    int32_t modelindex;
};
struct mstudiomodel_t {
    char name[64];
    int32_t type;
    float boundingradius;
    int32_t nummesh;
    int32_t meshindex;
    int32_t numverts;
    int32_t vertinfoindex;
    int32_t vertindex;
    int32_t numnorms;
    int32_t norminfoindex;
    int32_t normindex;
    int32_t numgroups;
    int32_t groupindex;
};
#pragma pack(pop)

constexpr int32_t VP20_BONE_BUDGET = 50; // fork-plan's own figure, see header comment

struct SubmodelResult {
    std::string modelFile;
    std::string bodypartName;
    std::string submodelName;
    int32_t numVerts = 0;
    int32_t distinctBones = 0;
};

// Reads the whole file so struct offsets (all file-relative in a
// non-sequence-group GoldSrc MDL) can be read directly, the same
// assumption MdlParser::parseTextures already makes for texture data.
bool readWholeFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return false;
    std::streamsize size = f.tellg();
    if (size <= 0)
        return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

bool censusOneModel(const fs::path& mdlPath, std::vector<SubmodelResult>& out) {
    std::vector<uint8_t> raw;
    if (!readWholeFile(mdlPath, raw))
        return false;

    if (raw.size() < sizeof(studiohdr_t))
        return false;
    studiohdr_t hdr;
    std::memcpy(&hdr, raw.data(), sizeof(studiohdr_t));

    if (hdr.id != 0x54534449) // "IDST" little-endian, deps/hlsdk/engine/studio.h
        return false; // texture-only T.mdl / sequence-group files fail this - skip, not a real body model

    for (int32_t bp = 0; bp < hdr.numbodyparts; ++bp) {
        size_t bpOff = static_cast<size_t>(hdr.bodypartindex) + bp * sizeof(mstudiobodyparts_t);
        if (bpOff + sizeof(mstudiobodyparts_t) > raw.size())
            break;
        mstudiobodyparts_t bodypart;
        std::memcpy(&bodypart, raw.data() + bpOff, sizeof(mstudiobodyparts_t));

        for (int32_t m = 0; m < bodypart.nummodels; ++m) {
            size_t modOff = static_cast<size_t>(bodypart.modelindex) + m * sizeof(mstudiomodel_t);
            if (modOff + sizeof(mstudiomodel_t) > raw.size())
                break;
            mstudiomodel_t model;
            std::memcpy(&model, raw.data() + modOff, sizeof(mstudiomodel_t));

            if (model.numverts <= 0)
                continue;
            size_t vertInfoOff = static_cast<size_t>(model.vertinfoindex);
            if (vertInfoOff + static_cast<size_t>(model.numverts) > raw.size())
                continue; // malformed/truncated - skip this submodel rather than read out of bounds

            std::set<uint8_t> distinct;
            for (int32_t v = 0; v < model.numverts; ++v)
                distinct.insert(raw[vertInfoOff + static_cast<size_t>(v)]);

            SubmodelResult r;
            r.modelFile = mdlPath.filename().string();
            r.bodypartName = bodypart.name;
            r.submodelName = model.name;
            r.numVerts = model.numverts;
            r.distinctBones = static_cast<int32_t>(distinct.size());
            out.push_back(r);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <models-dir> [<models-dir> ...]\n", argv[0]);
        return 1;
    }

    std::vector<SubmodelResult> all;
    size_t modelFiles = 0, notStudioModel = 0;

    for (int a = 1; a < argc; ++a) {
        fs::path root(argv[a]);
        if (!fs::is_directory(root)) {
            std::fprintf(stderr, "Not a directory, skipping: %s\n", argv[a]);
            continue;
        }
        std::vector<fs::path> mdls;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".mdl")
                mdls.push_back(entry.path());
        }
        std::sort(mdls.begin(), mdls.end());

        for (const auto& mdl : mdls) {
            ++modelFiles;
            std::vector<SubmodelResult> results;
            if (!censusOneModel(mdl, results)) {
                ++notStudioModel;
                continue;
            }
            for (auto& r : results)
                all.push_back(r);
        }
    }

    std::sort(all.begin(), all.end(), [](const SubmodelResult& a, const SubmodelResult& b) {
        return a.distinctBones > b.distinctBones;
    });

    std::printf("%-28s %-16s %-20s %8s %8s\n", "model", "bodypart", "submodel", "verts", "bones");
    size_t overBudget = 0;
    for (const auto& r : all) {
        bool over = r.distinctBones > VP20_BONE_BUDGET;
        if (over) {
            std::printf("%-28s %-16s %-20s %8d %8d  *** OVER %d ***\n", r.modelFile.c_str(),
                        r.bodypartName.c_str(), r.submodelName.c_str(), r.numVerts, r.distinctBones, VP20_BONE_BUDGET);
            ++overBudget;
        }
    }

    std::printf("\n%zu .mdl file(s) found, %zu were not a real body model (T.mdl/sequence-group/non-MDL, skipped)\n",
                modelFiles, notStudioModel);
    std::printf("%zu submodel(s) analysed across %zu real studio model file(s)\n", all.size(), modelFiles - notStudioModel);
    std::printf("%zu submodel(s) exceed the ~%d-bone VP20 budget and would need bone-subset partitioning\n",
                overBudget, VP20_BONE_BUDGET);
    if (!all.empty()) {
        std::printf("max distinct bones in any single submodel: %d (%s / %s / %s)\n",
                    all.front().distinctBones, all.front().modelFile.c_str(),
                    all.front().bodypartName.c_str(), all.front().submodelName.c_str());
    }

    return 0;
}
