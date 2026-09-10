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
// only ever touches a SUBSET of the submodel's vertices and normals.
// Counting distinct bones over the whole submodel can both overcount
// (combining meshes that never actually share a draw) and miscount
// which normals belong to which mesh.
//
// Both vertex- and normal-bones are now attributed exactly, per mesh -
// found in review, round 2: a first fix (still per-mesh for vertices)
// stopped short and unioned every normal-bone in the WHOLE submodel into
// every mesh as a "safe" over-approximation, unable to confirm from
// reading gl_studio.c alone whether the per-entry normindex (ptricmds[1])
// was mesh-local or already submodel-global. It is submodel-global,
// confirmed directly: R_StudioSetColorArray (gl_studio.c:1431-1436) reads
// `pstudionorms[ptricmds[1]]` and `g_studio.lightvalues[ptricmds[1]]`,
// the latter populated by a running index accumulated across ALL meshes
// in submodel order (the lighting-precompute loop just above it) - so a
// compiled MDL's own per-entry normindex already addresses the
// submodel's normal array directly, exactly parallel to how vertindex
// addresses its vertex array. This tool now walks each mesh's own
// triangle stream once and takes the union of (vertex-bones via
// vertindex -> vertinfoindex) and (normal-bones via normindex ->
// norminfoindex) for THAT mesh alone - the real, exact per-draw bone
// set, no over-approximation (GoldSrc's own per-submodel CPU skinning
// transforms every vertex/normal up front regardless of mesh boundaries;
// whether ref/nv2a would keep that structure or move skinning onto the
// GPU per mesh is unresolved and does not change what this measures -
// the mesh IS the real draw boundary either way).
//
// A mesh whose triangle stream is truncated or malformed is reported as
// UNANALYSED, not folded into the pass/fail counts as a low bone count -
// found in review: a swallowed read failure silently looking like a
// passing result is exactly the wrong direction for a census meant to
// show over-budget models are rare.
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
// the confirmed format) and inserts every referenced vertex AND normal
// index into the matching out-set. Both indices are used exactly as the
// reference does - ptricmds[0] into the submodel's vertex array,
// ptricmds[1] into its normal array, confirmed against
// R_StudioSetColorArray (gl_studio.c:1431-1436: `pstudionorms[ptricmds[1]]`
// and `g_studio.lightvalues[ptricmds[1]]`, the latter populated by a
// cumulative, submodel-wide running index across all meshes in mesh
// order - so a compiled MDL's own per-entry normal index already
// addresses the submodel-wide array directly, the same way vertindex
// does; no per-mesh remapping is needed, and no over-approximation - see
// the header comment on this file for what round 1 of review left
// unresolved here, closed by this exact reference read).
//
// Returns false (out sets left as-is, not necessarily complete) if the
// stream is truncated or malformed - a bounds failure or a missing
// terminator - so the caller can treat this mesh as UNANALYSED rather
// than silently reporting a partial, misleadingly-low bone count as if
// it were a real result. Found in review: the census exists to show
// over-budget models are rare, so a swallowed read failure producing a
// low count is exactly the wrong direction to fail in.
bool collectMeshIndices(const std::vector<uint8_t>& raw, size_t triOff,
                        std::set<uint16_t>& outVerts, std::set<uint16_t>& outNorms) {
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
            return false; // truncated before a terminator - real failure
        pos += sizeof(int16_t);
        if (header == 0)
            return true; // real terminator - a genuinely complete walk
        int count = header < 0 ? -header : header;
        for (int i = 0; i < count; ++i) {
            int16_t vertindex, normindex, s, t;
            if (!readI16(pos, vertindex)) return false;
            if (!readI16(pos + 2, normindex)) return false;
            if (!readI16(pos + 4, s)) return false;
            if (!readI16(pos + 6, t)) return false;
            (void)s; (void)t;
            if (vertindex >= 0)
                outVerts.insert(static_cast<uint16_t>(vertindex));
            if (normindex >= 0)
                outNorms.insert(static_cast<uint16_t>(normindex));
            pos += 4 * sizeof(int16_t);
        }
    }
}

struct SubmodelResult {
    std::string modelFile;
    std::string bodypartName;
    std::string submodelName;
    int32_t numVerts = 0;
    int32_t numMeshes = 0;
    int32_t peakBones = 0;      // max over meshes of exact per-mesh (vertex-bones UNION normal-bones)
    bool fullyAnalysed = true;  // false if ANY mesh's triangle stream failed to walk to its terminator
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
            size_t normInfoOff = static_cast<size_t>(model.norminfoindex);
            bool normArrayInBounds = model.numnorms > 0 &&
                normInfoOff + static_cast<size_t>(model.numnorms) <= raw.size();

            SubmodelResult sr;
            sr.modelFile = mdlPath.filename().string();
            sr.bodypartName = bodypart.name;
            sr.submodelName = model.name;
            sr.numVerts = model.numverts;
            sr.numMeshes = model.nummesh;

            for (int32_t mi = 0; mi < model.nummesh; ++mi) {
                size_t meshOff = static_cast<size_t>(model.meshindex) + mi * sizeof(mstudiomesh_t);
                if (meshOff + sizeof(mstudiomesh_t) > raw.size()) {
                    sr.fullyAnalysed = false;
                    break;
                }
                mstudiomesh_t mesh;
                std::memcpy(&mesh, raw.data() + meshOff, sizeof(mstudiomesh_t));

                std::set<uint16_t> vertIndices, normIndices;
                bool walkOk = collectMeshIndices(raw, static_cast<size_t>(mesh.triindex), vertIndices, normIndices);
                if (!walkOk) {
                    sr.fullyAnalysed = false;
                    continue; // this mesh's own count is unreliable - skip it, but keep checking the rest
                }

                std::set<uint8_t> bones;
                for (uint16_t vi : vertIndices) {
                    if (static_cast<size_t>(vi) < static_cast<size_t>(model.numverts))
                        bones.insert(raw[vertInfoOff + static_cast<size_t>(vi)]);
                    else
                        sr.fullyAnalysed = false; // out-of-range index the format shouldn't produce
                }
                for (uint16_t ni : normIndices) {
                    if (normArrayInBounds && static_cast<size_t>(ni) < static_cast<size_t>(model.numnorms))
                        bones.insert(raw[normInfoOff + static_cast<size_t>(ni)]);
                    else
                        sr.fullyAnalysed = false;
                }

                sr.peakBones = std::max(sr.peakBones, static_cast<int32_t>(bones.size()));
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

    // Failed-to-fully-analyse submodels are reported SEPARATELY, never
    // silently folded into the pass/near/over counts below - found in
    // review: a swallowed read failure producing a low bone count is
    // exactly the wrong direction for a census meant to show over-budget
    // models are rare.
    std::vector<SubmodelResult> analysed, failed;
    for (auto& r : all) {
        if (r.fullyAnalysed) analysed.push_back(r);
        else failed.push_back(r);
    }

    std::sort(analysed.begin(), analysed.end(), [](const SubmodelResult& a, const SubmodelResult& b) {
        return a.peakBones > b.peakBones;
    });

    // Full near-threshold distribution, not just a binary pass/fail - found
    // in review: a hardcoded exact cutoff hid how close everything else was.
    std::printf("%-28s %-16s %-20s %6s %6s %6s\n", "model", "bodypart", "submodel",
                "verts", "meshes", "bones");
    size_t overBudget = 0, nearBudget = 0; // "near" = within 10 of budget
    for (const auto& r : analysed) {
        bool over = r.peakBones > budget;
        bool near = !over && (budget - r.peakBones <= 10);
        if (over || near) {
            std::printf("%-28s %-16s %-20s %6d %6d %6d%s\n", r.modelFile.c_str(),
                        r.bodypartName.c_str(), r.submodelName.c_str(), r.numVerts, r.numMeshes,
                        r.peakBones, over ? "  *** OVER ***" : "  (near)");
            if (over) ++overBudget; else ++nearBudget;
        }
    }

    if (!failed.empty()) {
        std::printf("\n%zu submodel(s) could NOT be fully analysed (malformed/truncated triangle\n"
                    "  stream or an out-of-range index) - excluded from the counts below, not\n"
                    "  counted as passing:\n", failed.size());
        for (const auto& r : failed) {
            std::printf("  %s / %s / %s (partial peak seen before the failure: %d)\n",
                        r.modelFile.c_str(), r.bodypartName.c_str(), r.submodelName.c_str(), r.peakBones);
        }
    }

    std::printf("\nbudget: %d bones (override with --budget N; the fork-plan's own figure is an\n"
                "  ESTIMATE pending a real VP20 shader constant layout, not a verified exact cutoff)\n", budget);
    std::printf("%zu .mdl file(s) found, %zu were not a real body model (T.mdl/sequence-group/non-MDL, skipped)\n",
                modelFiles, notStudioModel);
    std::printf("%zu submodel(s) fully analysed across %zu real studio model file(s) (%zu could not be)\n",
                analysed.size(), modelFiles - notStudioModel, failed.size());
    std::printf("%zu submodel(s) exceed the budget (exact per-mesh bones: the union of that mesh's own\n"
                "  vertex- and normal-bone indices, both read directly from its triangle command stream)\n",
                overBudget);
    std::printf("%zu more submodel(s) are within 10 bones of the budget - worth re-checking once the real shader\n"
                "  constant layout narrows the estimate\n", nearBudget);
    if (!analysed.empty()) {
        std::printf("max exact bones in any single mesh: %d (%s / %s / %s)\n",
                    analysed.front().peakBones, analysed.front().modelFile.c_str(),
                    analysed.front().bodypartName.c_str(), analysed.front().submodelName.c_str());
    }

    return failed.empty() ? 0 : 1;
}
