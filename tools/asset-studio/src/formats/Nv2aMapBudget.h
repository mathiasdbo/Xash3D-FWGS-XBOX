#pragma once
#include "MpakHeader.h"
#include <cstdint>
#include <ostream>

namespace formats {

    // ref/nv2a's own per-map memory ledger, mirrored at bake time so an
    // over-budget map fails the BAKE instead of silently failing to draw
    // its real geometry on the console (nv2a_load_map_geometry falls back
    // to the X9-A bring-up triangle and Con_Printf's, it never crashes -
    // r_context.c nv2a_load_map_geometry). Same duplicated-literal reasoning as the
    // NV2A_MAP_MAX_TEXTURES/NV2A_MAP_MAX_BATCHES caps a few lines above
    // this block in PakManager.cpp: the constant and the formula are both
    // duplicated on purpose (moving them into a shared C/C++ header like
    // filesystem/mpak_texture_dir.h is a possible follow-up), with this cross-reference comment as the thing
    // that keeps them in sync - if r_context.c's cap or ledger ever change,
    // update this file too.
    //
    // What this check does NOT cover: r_context.c's own NV2A_MAP_BUDGET_BYTES
    // comment (r_context.c nv2a_load_map_geometry) is explicit that this is a coarse
    // guard against a doomed giant allocation, not a real accounting of the
    // Xbox's whole 64 MiB budget - the engine's own static state, pbkit's
    // front/back/depth buffers, every other subsystem's allocations, and
    // allocator fragmentation are all real, all already resident by the
    // time nv2a_load_map_geometry runs, and none of them are visible to
    // this narrow per-map ledger. This header mirrors that same ledger and
    // nothing more.
    constexpr uint64_t kNv2aMapBudgetBytes = 24ull * 1024ull * 1024ull; // ref/nv2a/r_context.c NV2A_MAP_BUDGET_BYTES

    // sizeof(nv2a_lit_vertex_t) - the RUNTIME-expanded, packed vertex the
    // renderer actually builds in memory (float pos[3], uv0[2], uv1[2] =
    // 7 floats = 28 bytes, ref/nv2a/r_context.c nv2a_lit_vertex_t), NOT sizeof(Vertex)
    // / mpak_vertex_t, the smaller on-disk record this baker writes to the
    // archive. The ledger below is a mirror of the console's real per-map
    // RAM cost, not of anything already in the file.
    constexpr uint64_t kNv2aLitVertexBytes = 28;

    // Mirrors nv2a_load_map_geometry's own ledger sum (r_context.c nv2a_load_map_geometry),
    // built from the two byte counts computed just above it (r_context.c:
    // 3074-3076: dword_count = (iCount+1)/2, vb_bytes = vCount * sizeof(
    // nv2a_lit_vertex_t), idx_bytes = dword_count * sizeof(uint32_t)) plus
    // the archive's own on-disk size (buffersize - real, already-resident
    // cost by the time nv2a_load_map_geometry runs; Mod_ProcessRenderData
    // passes it through from FS_LoadFile's own length, engine/common/
    // model.c:330,387).
    //
    // header.lenX is used directly for every geo.X.length term instead of
    // re-deriving geo via Mpak_ComputeLayout: filesystem/mpak_layout.c:167
    // stores the section's DECLARED length verbatim in Mpak_PlaceLump, not
    // a sector-rounded one, so geo.X.length == header.lenX exactly - the
    // header fields this function is handed are the same bytes the ref
    // would read.
    //
    // Every term is computed in uint64 here, where the ref computes
    // dword_count in a uint32. The two are identical for any iCount below
    // 2^32-1: iCount is mIndices.size(), a count of uint16_t indices, which
    // never approaches that range for a GoldSrc map. Noted, not "fixed" -
    // matching the ref's own uint32 dword_count would just reintroduce the
    // overflow risk this baker has no reason to have.
    // ref/nv2a's nv2a_map_face_t, one per face (on-disk FaceDrawCmd is 8
    // bytes); r_context.c STATIC_ASSERTs the same 52 so the two cannot drift.
    constexpr uint64_t kNv2aMapFaceBytes = 52;

    inline uint64_t nv2aMapLedgerBytes(const MpakHeader6& h, uint64_t fileSize) {
        const uint64_t vbBytes = static_cast<uint64_t>(h.vCount) * kNv2aLitVertexBytes;
        const uint64_t idxBytes = ((static_cast<uint64_t>(h.iCount) + 1) / 2) * 4;
        return vbBytes + idxBytes
            + static_cast<uint64_t>(h.lenFaceDrawCmds / 8) * kNv2aMapFaceBytes
            + static_cast<uint64_t>(h.lenFaceDrawCmds) / 8
            + static_cast<uint64_t>(h.lenFaceRemap) * 2
            + static_cast<uint64_t>(h.lenTextureData)
            + static_cast<uint64_t>(h.lenLightmapAtlas)
            + fileSize;
    }

    // Mirrors r_context.c nv2a_load_map_geometry's strict `total_bytes > NV2A_MAP_BUDGET_BYTES`
    // - exactly-at-budget PASSES on the console, so it must pass here too.
    inline bool nv2aMapWithinBudget(uint64_t ledgerBytes) {
        return !(ledgerBytes > kNv2aMapBudgetBytes);
    }

} // namespace formats
