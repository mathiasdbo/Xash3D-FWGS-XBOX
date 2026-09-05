#pragma once
#include <cstdint>

namespace formats {

    // Bumped whenever the baker's OUTPUT changes in a way that doesn't itself
    // change `version` - e.g. a resampling bug fix, a WAD-priority tie-break
    // change - anything where two archives can legitimately both claim the
    // same `version` yet contain meaningfully different bytes. The shipped
    // `global.mpak` proved this failure mode: it reports v5 (accepted by any
    // v5 reader) while its content demonstrably came from an earlier baker
    // revision than the one that shipped it. `version` alone can't catch
    // that; this can, by giving every rebake a distinguishable stamp even
    // when the on-disk *format* hasn't changed.
    constexpr uint32_t MPAK_BAKER_BUILD_ID = 1;

    // MPAKv6 fixed-size header, written verbatim (2048-byte padded) at the start of
    // every .mpak archive. Both PakManager (BSP maps / global asset packs) and
    // UiPacker (menu backgrounds) serialize this same struct so the Rust reader
    // (filesystem/mpak.c, not written yet) only has to know one layout.
    //
    // IMPORTANT: this format has no offset table - byte layout order IS the format,
    // and the Rust reader hardcodes these offsets. New fields may only be appended
    // after lenTexinfo; never reorder or remove existing fields.
    //
    // v5 -> v6: two real layout changes, which is why (unlike v4->v5) this bump
    // touches the header struct itself, not just what the bytes mean:
    //  - `lenLighting`/the raw LUMP_LIGHTING body it sized are GONE. Nothing has
    //    ever read them at runtime (the baked RGBA8 lightmap atlas superseded
    //    this lump entirely) - grep-confirmed dead weight, ~376 KB/map of it.
    //  - Two new lumps: `lenFaceInfo` (one `MpakFaceInfo` per face - flags,
    //    plane index, texinfo index, lightstyles, lightmap placement - all
    //    computed transiently during face baking today and then discarded) and
    //    `lenTexinfo` (the raw LUMP_TEXINFO passthrough `MpakFaceInfo::texinfo`
    //    indexes into, for its `vecs[2][4]` - real GoldSrc/Xash's own
    //    `Mod_LightMatrixFromTexMatrix`, `mod_bmodel.c:1553-1559` (called from
    //    `Mod_CalcSurfaceExtents`, `:1603`), copies `lmvecs` from
    //    `texinfo->vecs` verbatim for every real map - the function's own
    //    early return at `:1562` only skips this for `TEX_WORLD_LUXELS`
    //    content, which doesn't exist in retail HL1 BSPs
    //    content exists in retail HL1 BSPs - so a per-face vector copy would
    //    just duplicate this lump's own data field-for-field).
    //  A v5 reader has always hard-rejected anything but `version == 5`
    //  (`filesystem/mpak.c`, not written yet), so - same as every version bump before this
    //  one - there is no partial-compatibility burden: every existing archive
    //  needs a full rebake regardless, which is also why removing a field
    //  outright (rather than leaving a permanently-zero husk) is safe here.
    //
    // Every field has an explicit default. Only one target branch (PC vs Xbox) of
    // each writer ever assigns most of these, and a bare `MpakHeader6 header;`
    // (default-initialization) would leave the untouched branch's fields as
    // indeterminate stack garbage - which previously became a garbage lump length
    // read back by an offset-table-less format. Always construct as
    // `MpakHeader6 header{};` (value-initialization) regardless; the NSDMIs below
    // are a second line of defense.
    struct MpakHeader6 {
        char magic[4] = {'M', 'P', 'A', 'K'};
        uint32_t version = 6;
        uint32_t headerSize = 0;      // sizeof(MpakHeader6); set in PakManager::save just before writing
        uint32_t bakerBuildId = MPAK_BAKER_BUILD_ID;
        uint32_t flags = 0; // 1 = Xbox, 0 = PC

        uint32_t vCount = 0;
        uint32_t iCount = 0;

        uint32_t lenVisibility = 0;
        uint32_t lenNodes = 0;
        uint32_t lenLeaves = 0;
        uint32_t lenMarksurfaces = 0;
        uint32_t lenFaceDrawCmds = 0;
        uint32_t lenPlanes = 0;
        uint32_t lenClipnodes = 0;
        uint32_t lenModels = 0;

        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        uint32_t lenAtlas = 0;

        uint32_t count64 = 0;
        uint32_t lenArray64 = 0;
        uint32_t count256 = 0;
        uint32_t lenArray256 = 0;
        uint32_t count512 = 0;
        uint32_t lenArray512 = 0;

        uint32_t lenAssetDict = 0;
        uint32_t lenAssetData = 0;

        uint32_t lenEntities = 0;       // raw LUMP_ENTITIES ASCII text, NUL-terminated

        uint32_t lightmapWidth = 0;     // per-page luxel width (e.g. 1024)
        uint32_t lightmapHeight = 0;    // per-page luxel height (e.g. 1024)
        uint32_t lightmapLayers = 0;    // number of atlas pages
        uint32_t lightmapFormat = 0;    // 0 = RGBA8; reserved for RGB565/ARGB1555 (Xbox)
        uint32_t lenLightmapAtlas = 0;  // total byte length of the layer-major atlas

        // --- MPAKv6 additions (appended after lenLightmapAtlas; append-only from here) ---
        uint32_t lenFaceInfo = 0;       // MpakFaceInfo[numFaces], parallel to face_draw_cmds
        uint32_t lenTexinfo = 0;        // raw LUMP_TEXINFO, MpakFaceInfo::texinfo indexes into it
    };

    // GoldSrc BSP30 SURF_* flags (common/bspfile.h in the canonical Xash3D-FWGS
    // clone) - the subset that is STATIC per-face (decidable at bake time from
    // the face's texture name and texinfo, not from which leaf the viewer is
    // standing in). SURF_UNDERWATER is deliberately not modeled here: the real
    // engine sets it per-leaf-content at runtime (mod_bmodel.c:3631), not at
    // face-load time, so it isn't a fact this baker can bake.
    constexpr uint32_t SURF_PLANEBACK = 0x0002; // dface_t::side != 0 - negate the plane test
    constexpr uint32_t SURF_DRAWSKY = 0x0004;   // texture name starts with "sky"
    constexpr uint32_t SURF_DRAWTURB = 0x0010;  // texture name looks like water (starts '*' or '!')
    constexpr uint32_t SURF_DRAWTILED = 0x0020; // texinfo.flags & TEX_SPECIAL - no lightmap
    constexpr uint32_t SURF_CONVEYOR = 0x0040;  // texture name starts with "scroll" - UV-scrolled at render time
    constexpr uint32_t SURF_TRANSPARENT = 0x0100; // texture name starts with '{' - masked/keyed

    // On-disk `MpakFaceInfo` - 44 bytes, one per face, index-parallel with
    // `face_draw_cmds`/`LUMP_FACES`'s own original order. Everything here is
    // computed transiently by BspParser::exportToPak's existing face loop
    // today and then thrown away; this just keeps it. `mextrasurf_t`'s real
    // `lmvecs` is deliberately NOT duplicated here - see MpakHeader6's v6 doc
    // comment above for why `texinfo` (an index into the new raw LUMP_TEXINFO
    // lump) is enough.
#pragma pack(push, 1)
    struct MpakFaceInfo {
        uint32_t flags = 0;            // SURF_* bits above
        int32_t planenum = -1;         // index into the planes lump (dface_t::planenum)
        int32_t texinfo = -1;          // index into the new raw LUMP_TEXINFO lump, -1 if the source face had none
        uint8_t styles[4] = {255, 255, 255, 255}; // dface_t::styles verbatim, 255 = unused
        int32_t lightmapmins[2] = {0, 0}; // texture-space units (Mod_CalcSurfaceExtents's texturemins)
        int32_t extents[2] = {0, 0};      // texture-space units ((bmaxs-bmins)*16), 0 if no real lightmap
        uint32_t lightmapPage = 0;        // which atlas page this face's lightmap block lives on
        uint32_t lightmapBlockX = 0;      // bordered block origin within that page (luxels)
        uint32_t lightmapBlockY = 0;
    };
#pragma pack(pop)
    static_assert(sizeof(MpakFaceInfo) == 44, "MpakFaceInfo must stay a stable, tightly packed 44 bytes - the Rust reader computes numFaces from lenFaceInfo / 44");

} // namespace formats
