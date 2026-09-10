// XB census #2 (docs/r3d/fork-plan.md, "Two censuses first"): which
// retail models reference more bones per DRAW than a VP20 vertex shader
// palette holds - 192 constants total (deps/nxdk/tools/vp20compiler/
// config.h), three per bone, roughly 50 left after the view-projection
// and light constants a shader also needs (approximate - the exact
// reservation depends on a shader that does not exist yet; see
// --budget below), against MAXSTUDIOBONES 128 (deps/hlsdk/engine/
// studio.h). Every draw over the line needs partitioning by bone
// subset before it can be rendered on the NV2A; the census is what
// says which ones, not a design this tool makes.
//
// GoldSrc studio models rig each vertex to exactly ONE bone (not a
// weighted multi-bone skin): mstudiomodel_t::vertinfoindex points to
// numverts bytes, one per vertex, each the index of the bone it's rigid
// to (deps/hlsdk/engine/studio.h's own mstudiomodel_t comment: "vertex
// bone info").
//
// PER-MESH, NOT PER-SUBMODEL - found in Codex review, round 1. A
// submodel (mstudiomodel_t) is not one draw: the reference (ref/gl/
// gl_studio.c's R_StudioDrawPoints, :2078-2112) submits each
// mstudiomesh_t separately, and a mesh's own triangle command stream
// (mesh->triindex: int16 run-length headers - positive strip, negative
// fan, magnitude = vertex count in that run, 0 terminates - each run's
// entries four int16s {vertindex, normindex, s, t}, confirmed against
// R_StudioBuildArrayNormalMesh, gl_studio.c:1749-1777, not guessed)
// only ever touches a SUBSET of the submodel's vertices. Counting
// distinct bones over the whole submodel can both overcount (combining
// meshes that never actually share a draw) and, in principle, miss
// bones a mesh's own normals reference that the census never looked
// at. This tool now walks each mesh's real triangle command stream and
// counts DISTINCT vertex-bones per mesh - the real per-draw figure for
// the dominant cost (GoldSrc's own per-submodel CPU skinning transforms
// every vertex up front regardless, so vertex-bone attribution is
// exact here; whether ref/nv2a would keep that structure or move
// skinning onto the GPU per mesh is unresolved and does not change what
// this measures).
//
// Normals are deliberately NOT attributed per mesh: whether the
// reference's own per-mesh submission loop reads a mesh-local or a
// cumulatively-advanced normal-bone pointer across the whole submodel
// was not resolved by reading gl_studio.c's two separate mesh loops
// (lighting precompute vs. triangle submission) to full confidence in
// the time this census took, and AGENTS.md's "do not guess" rule means
// this tool does not pretend otherwise. Instead, every normal-bone in
// the WHOLE submodel (mstudiomodel_t::norminfoindex/numnorms) is added
// to every mesh's own count - a safe over-approximation for the
// dominant vertex cost. If a submodel passes even under this
// pessimistic assumption, it is real headroom; if it only fails
// because of the over-approximation, that is visible in the reported
// vertex-only figure printed alongside it.
//
// Usage: census_bones [--budget N] <models-dir> [<models-dir> ...]
// --budget overrides the ~50 estimate (default 50) - found in review:
// the real number depends on a shader that does not exist yet, so a
// hardcoded exact cutoff overstated this census's own precision.
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
// mstudiomodel_t :277-299, mstudiomesh_t :306-313), not guessed.
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
struct mstudiomesh_t {
    int32_t numtris;
    int32_t triindex;
    int32_t skinref;
    int32_t numnorms;
    int32_t normindex;
};
#pragma pack(pop)

// Walks one mesh's real triangle command stream (see header comment for
// the confirmed format) and inserts every referenced vertex index into
// out. Bounds-checked against raw.size() at every read - a malformed or
// truncated stream stops the walk rather than reading out of bounds.
void collectMeshVertIndices(const std::vector<uint8_t>& raw, size_t triOff, std::set<uint16_t>& out) {
    size_t pos = triOff;
    auto readI16 = [&](size_t off, int16_t& v) -> bool {
        if (off + sizeof(int16_t) > raw.size())
            return false;
        std::memcpy(&v, raw.data() + off, sizeof(int16_t));
        return true;
    };

    for (;;) {
        int16_t header;
        if (!readI16(pos, header))
            return; // truncated - stop rather than read out of bounds
        pos += sizeof(int16_t);
        if (header == 0)
            return; // real terminator
        int count = header < 0 ? -header : header;
        for (int i = 0; i < count; ++i) {
            int16_t vertindex, normindex, s, t;
            if (!readI16(pos, vertindex)) return;
            if (!readI16(pos + 2, normindex)) return;
            if (!readI16(pos + 4, s)) return;
            if (!readI16(pos + 6, t)) return;
            (void)normindex; (void)s; (void)t;
            if (vertindex >= 0)
                out.insert(static_cast<uint16_t>(vertindex));
            pos += 4 * sizeof(int16_t);
        }
    }
}

struct MeshResult {
    int32_t meshIndex = 0;
    int32_t vertexOnlyBones = 0;   // distinct bones among THIS mesh's own referenced vertices - exact
    int32_t withNormalBones = 0;   // above, unioned with the WHOLE submodel's normal-bones - conservative upper bound
};

struct SubmodelResult {
    std::string modelFile;
    std::string bodypartName;
    std::string submodelName;
    int32_t numVerts = 0;
    int32_t numMeshes = 0;
    int32_t peakVertexOnlyBones = 0; // max over meshes of vertexOnlyBones - the real, exact per-draw figure
    int32_t peakWithNormalBones = 0; // max over meshes of withNormalBones - conservative upper bound
};

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

    if (hdr.id != 0x54534449) // "IDST" little-endian - see MdlParser.cpp's own check
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

            if (model.numverts <= 0 || model.nummesh <= 0)
                continue;
            size_t vertInfoOff = static_cast<size_t>(model.vertinfoindex);
            if (vertInfoOff + static_cast<size_t>(model.numverts) > raw.size())
                continue;

            // Every normal-bone in the whole submodel, added to every mesh's
            // own count as a conservative over-approximation - see header
            // comment for why this is not attributed per mesh.
            std::set<uint8_t> submodelNormBones;
            if (model.numnorms > 0) {
                size_t normInfoOff = static_cast<size_t>(model.norminfoindex);
                if (normInfoOff + static_cast<size_t>(model.numnorms) <= raw.size()) {
                    for (int32_t n = 0; n < model.numnorms; ++n)
                        submodelNormBones.insert(raw[normInfoOff + static_cast<size_t>(n)]);
                }
            }

            SubmodelResult sr;
            sr.modelFile = mdlPath.filename().string();
            sr.bodypartName = bodypart.name;
            sr.submodelName = model.name;
            sr.numVerts = model.numverts;
            sr.numMeshes = model.nummesh;

            for (int32_t mi = 0; mi < model.nummesh; ++mi) {
                size_t meshOff = static_cast<size_t>(model.meshindex) + mi * sizeof(mstudiomesh_t);
                if (meshOff + sizeof(mstudiomesh_t) > raw.size())
                    break;
                mstudiomesh_t mesh;
                std::memcpy(&mesh, raw.data() + meshOff, sizeof(mstudiomesh_t));

                std::set<uint16_t> vertIndices;
                collectMeshVertIndices(raw, static_cast<size_t>(mesh.triindex), vertIndices);

                std::set<uint8_t> vertBones;
                for (uint16_t vi : vertIndices) {
                    if (static_cast<size_t>(vi) < static_cast<size_t>(model.numverts))
                        vertBones.insert(raw[vertInfoOff + static_cast<size_t>(vi)]);
                }

                std::set<uint8_t> withNorms = vertBones;
                withNorms.insert(submodelNormBones.begin(), submodelNormBones.end());

                sr.peakVertexOnlyBones = std::max(sr.peakVertexOnlyBones, static_cast<int32_t>(vertBones.size()));
                sr.peakWithNormalBones = std::max(sr.peakWithNormalBones, static_cast<int32_t>(withNorms.size()));
            }

            out.push_back(sr);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int32_t budget = 50; // fork-plan's own estimate; override with --budget
    std::vector<std::string> dirs;
    for (int a = 1; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--budget" && a + 1 < argc) {
            budget = std::atoi(argv[++a]);
        } else {
            dirs.push_back(arg);
        }
    }
    if (dirs.empty()) {
        std::fprintf(stderr, "Usage: %s [--budget N] <models-dir> [<models-dir> ...]\n", argv[0]);
        return 1;
    }

    std::vector<SubmodelResult> all;
    size_t modelFiles = 0, notStudioModel = 0;

    for (const auto& dirArg : dirs) {
        fs::path root(dirArg);
        if (!fs::is_directory(root)) {
            std::fprintf(stderr, "Not a directory, skipping: %s\n", dirArg.c_str());
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
        return a.peakWithNormalBones > b.peakWithNormalBones;
    });

    // Full near-threshold distribution, not just a binary pass/fail - found
    // in review: a hardcoded exact cutoff hid how close everything else was.
    std::printf("%-28s %-16s %-20s %6s %6s %10s %10s\n", "model", "bodypart", "submodel",
                "verts", "meshes", "peak-vert", "peak+norm");
    size_t overBudget = 0, nearBudget = 0; // "near" = within 10 of budget, either measure
    for (const auto& r : all) {
        bool over = r.peakWithNormalBones > budget;
        bool near = !over && (budget - r.peakWithNormalBones <= 10);
        if (over || near) {
            std::printf("%-28s %-16s %-20s %6d %6d %10d %10d%s\n", r.modelFile.c_str(),
                        r.bodypartName.c_str(), r.submodelName.c_str(), r.numVerts, r.numMeshes,
                        r.peakVertexOnlyBones, r.peakWithNormalBones, over ? "  *** OVER ***" : "  (near)");
            if (over) ++overBudget; else ++nearBudget;
        }
    }

    std::printf("\nbudget: %d bones (override with --budget N; the fork-plan's own figure is an\n"
                "  ESTIMATE pending a real VP20 shader constant layout, not a verified exact cutoff)\n", budget);
    std::printf("%zu .mdl file(s) found, %zu were not a real body model (T.mdl/sequence-group/non-MDL, skipped)\n",
                modelFiles, notStudioModel);
    std::printf("%zu submodel(s) analysed across %zu real studio model file(s)\n", all.size(), modelFiles - notStudioModel);
    std::printf("%zu submodel(s) exceed the budget (peak per-mesh bones, vertices + conservatively all submodel normals)\n",
                overBudget);
    std::printf("%zu more submodel(s) are within 10 bones of the budget - worth re-checking once the real shader\n"
                "  constant layout narrows the estimate\n", nearBudget);
    if (!all.empty()) {
        std::printf("max peak bones in any single mesh (with conservative normal over-approximation): %d (%s / %s / %s)\n",
                    all.front().peakWithNormalBones, all.front().modelFile.c_str(),
                    all.front().bodypartName.c_str(), all.front().submodelName.c_str());
    }

    return 0;
}
