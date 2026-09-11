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
    constexpr uint32_t MPAK_BAKER_BUILD_ID = 2;

    // MpakHeader6::flags bits.
    constexpr uint32_t MPAK_FLAG_XBOX = 1;        // Xbox target: DXT atlas instead of PC texture arrays
    constexpr uint32_t MPAK_FLAG_NV2A_BLOCK = 2;  // v7: the baked Vertex/index/FaceDrawCmd/FaceInfo/
                                                  // DXT/lightmap-atlas block is populated (X9 profile);
                                                  // clear = core profile, every one of those lengths is 0

    // v7 first sector. A map archive IS a BSP30 file: offset 0 holds a real
    // GoldSrc dheader_t (version 30, fifteen {offset, length} pairs pointing
    // at the sector-aligned raw lumps below), because the engine validates a
    // map FROM DISK before any model loader runs - SV_ValidateMap ->
    // SV_ReadEntityScript -> Mod_TestBmodelLumps (sv_game.c:823-853,
    // mod_bmodel.c:4323-4398), plus Mod_CheckLump/ReadLump/SaveLump and the
    // map-list check in con_utils.c:93 - and every one of them reads the
    // first bytes of "maps/<name>.bsp" expecting exactly this struct
    // (docs/r3d/mpak-loading-design.md, F7). The MpakHeader6 descriptor
    // (this tool's own header) moves to MPAK_DESCRIPTOR_OFFSET; bytes
    // 124..127 stay zero so the engine's dextrahdr_t probe at
    // sizeof(dheader_t) reads id == 0, never IDEXTRAHEADER. The rest of the
    // sector is zero. filesystem/mpak.c probes both structs;
    // filesystem/mpak_layout.c checks the two describe the same bytes.
    constexpr uint32_t MPAK_DESCRIPTOR_OFFSET = 128;
    constexpr uint32_t MPAK_BSP_VERSION = 30;       // HLBSP_VERSION, common/bspfile.h
    constexpr uint32_t MPAK_BSP_LUMPS = 15;         // HEADER_LUMPS, common/bspfile.h

    // Mirrors the engine's dheader_t/dlump_t (common/bspfile.h) byte for
    // byte - int32 version, then HEADER_LUMPS {fileofs, filelen} pairs.
    struct MpakBspLump {
        int32_t fileofs = 0;
        int32_t filelen = 0;
    };
    struct MpakBspHeader {
        int32_t version = static_cast<int32_t>(MPAK_BSP_VERSION);
        MpakBspLump lumps[MPAK_BSP_LUMPS];
    };
    static_assert(sizeof(MpakBspHeader) == 124, "must match the engine's dheader_t exactly");
    static_assert(MPAK_DESCRIPTOR_OFFSET >= sizeof(MpakBspHeader) + 4, "descriptor must leave the dextrahdr_t probe bytes zero");

    // MPAKv6 fixed-size header (the "descriptor"), written verbatim inside the
    // first 2048-byte sector of every .mpak archive - for map archives at
    // MPAK_DESCRIPTOR_OFFSET (see the v7 first-sector note above), for
    // UiPacker's menu archives still at offset 0 (they are not maps and no
    // engine path reads them yet). PakManager (BSP maps) and UiPacker (menu
    // backgrounds) serialize this same struct so the engine-side C reader
    // (filesystem/mpak.c, X2 - see docs/r3d/fork-plan.md) only has to know
    // one layout.
    //
    // Corrected 2026-09: earlier comments here said "the Rust reader" -
    // stale, left over from this repo's own pre-rename bootstrap (this
    // project's real engine is Xash3D-FWGS in C, not Rust; see git log's
    // very first commit). filesystem/mpak.c is, and always was meant to
    // be, a C file.
    //
    // Scope, X2: .mpak is deliberately limited to BSP geometry, level
    // (WAD-resolved) textures, and lightmaps - the genuinely sequential-
    // I/O-shaped data a monolithic per-map archive earns its keep for on
    // the Xbox's slow IDE bus. Models, sprites and sounds are ordinary,
    // already-self-contained GoldSrc files upstream's own loaders already
    // read - main.cpp no longer parses or embeds them here at all, it
    // writes their names to a sibling <mapname>.assets.txt manifest
    // instead (a real, checked build step whose deployment consumer -
    // staging those loose files onto whatever the engine's search path
    // ends up being - is separate work, not this tool's job). lenAssetDict
    // and lenAssetData below are consequently always 0 for every archive
    // this tool produces now - kept in the struct (append-only, see
    // IMPORTANT below) rather than removed, since removing a field is a
    // version bump this scope change does not otherwise need.
    //
    // v7 -> v8: multi-lightstyle NV2A atlas blocks (docs/r3d/fork-plan.md's
    // XB section, "Lightstyles" census). BspParser.cpp's own baked atlas
    // has only ever held ONE lightmap block per face (`dface_t::styles[0]`),
    // even though `MpakFaceInfo::styles[4]` already carried all four style
    // IDs verbatim from v6 onward and LUMP_LIGHTING (raw-passthrough since
    // v7) has always held the samples for every one of them. Measured
    // against the real retail install: 6.69% of lit faces across 82% of
    // maps use a second style, so dropping them from the ONE renderer that
    // reads the atlas (the future X9 `ref/nv2a`) is a real, visible
    // regression, not an edge case - the census exists specifically to
    // settle this rather than guess it. Nothing before X9 reads the atlas
    // at all (classic-core-profile PC targets and the core-profile-only
    // Xbox path use the same raw LUMP_LIGHTING passthrough `ref/gl` always
    // has, unaffected either way), so this is a pure additive format
    // change: one new field, one new sparse section, no existing field's
    // meaning or layout disturbed. `MpakExtraLightBlock` below holds one
    // entry per face PER EXTRA STYLE it uses (not a fixed 4-slot-per-face
    // table) - most faces use zero, keeping the archive lean for the ~93%
    // that don't need it. `lenExtraLightBlocks` (appended below, after
    // `lenMiptex`, same append-only rule as every prior version) is 0 in
    // the core profile and whenever a real bake finds no multi-style face.
    //
    // v6 -> v7, the "classic core" (docs/r3d/mpak-loading-design.md):
    // auditing every real consumer of the runtime BSP structures showed
    // that nothing before the X9 hardware renderer can use the baked
    // Vertex/index/FaceDrawCmd/lightmap-atlas set at all - ref/soft
    // scan-converts from LUMP_SURFEDGES/EDGES every frame (r_rast.c:444),
    // the server and both renderers rebuild lighting per lightstyle and
    // per dynamic light from per-face LUMP_LIGHTING samples
    // (sv_world.c:1546-1571, r_surf.c:174-236), and pm_trace/sv_world need
    // texture NAMES (pm_trace.c:835, sv_world.c:1436). So v7 appends raw
    // LUMP_VERTEXES/EDGES/SURFEDGES/FACES/LIGHTING plus a TEXTURES lump in
    // the embedded-miptex shape compilers emit for -nowadtextures (names,
    // dims, 8-bit mips, palette - resolved from the WADs here at bake
    // time), and the engine loads the whole archive through its
    // UNMODIFIED Mod_LoadBmodelLumps by writing a synthetic dheader_t over
    // this header's own sector. The baked v6 block becomes optional
    // (MPAK_FLAG_NV2A_BLOCK) - present only in the X9 profile, every one of
    // its lengths 0 otherwise. `lenLighting` returns after being removed
    // in v6 as "dead weight": it was only dead while the atlas was assumed
    // to be the sole lighting source, which the audit above disproved.
    //
    // IMPORTANT: this descriptor has no offset table - byte layout order IS the
    // format, and the C reader (filesystem/mpak_layout.c) walks the sections in
    // write order. New fields may only be appended at the end; never reorder
    // or remove existing fields. (v7's dheader_t at offset 0 IS an offset
    // table, but only for the fifteen classic lumps and only for the engine's
    // benefit - the two are written from one cursor and the reader checks
    // they agree; the descriptor stays the source of truth for the rest.)
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
        uint32_t version = 8;
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

        uint32_t lenAssetDict = 0;  // always 0 now - see the X2 scope note above
        uint32_t lenAssetData = 0;  // always 0 now - see the X2 scope note above

        uint32_t lenEntities = 0;       // raw LUMP_ENTITIES ASCII text, NUL-terminated

        uint32_t lightmapWidth = 0;     // per-page luxel width (e.g. 1024)
        uint32_t lightmapHeight = 0;    // per-page luxel height (e.g. 1024)
        uint32_t lightmapLayers = 0;    // number of atlas pages
        uint32_t lightmapFormat = 0;    // 0 = RGBA8; reserved for RGB565/ARGB1555 (Xbox)
        uint32_t lenLightmapAtlas = 0;  // total byte length of the layer-major atlas

        // --- MPAKv6 additions (appended after lenLightmapAtlas; append-only from here) ---
        uint32_t lenFaceInfo = 0;       // MpakFaceInfo[numFaces], parallel to face_draw_cmds
        uint32_t lenTexinfo = 0;        // raw LUMP_TEXINFO, MpakFaceInfo::texinfo indexes into it

        // --- MPAKv7 additions: the classic core (see the v6 -> v7 note above) ---
        // Written in this order, after lenTexinfo, each sector-padded like
        // every other section. filesystem/mpak_layout.c mirrors these
        // exactly; change one, change the other.
        uint32_t lenVertexes = 0;       // raw LUMP_VERTEXES (dvertex_t[])
        uint32_t lenEdges = 0;          // raw LUMP_EDGES (dedge_t[])
        uint32_t lenSurfedges = 0;      // raw LUMP_SURFEDGES (int32[])
        uint32_t lenFaces = 0;          // raw LUMP_FACES (dface_t[], 20 bytes each)
        uint32_t lenLighting = 0;       // raw LUMP_LIGHTING (color24, every lightstyle, dface_t::lightofs intact)
        uint32_t lenMiptex = 0;         // embedded-miptex LUMP_TEXTURES: dmiptexlump_t + miptex_t + mips + palette

        // --- MPAKv8 addition (see the v7 -> v8 note above; nv2a profile only) ---
        uint32_t lenExtraLightBlocks = 0; // MpakExtraLightBlock[], one per face per extra (non-style-0) lightstyle
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
    static_assert(sizeof(MpakFaceInfo) == 44, "MpakFaceInfo must stay a stable, tightly packed 44 bytes - the C reader (filesystem/mpak_layout.c) computes numFaces from lenFaceInfo / 44");

    // v8: one entry per face PER EXTRA (non-style-0) lightstyle it uses -
    // see MpakHeader6's v7 -> v8 note above for why this is sparse rather
    // than a fixed 4-slot table. `MpakFaceInfo::styles[styleSlot]` already
    // names which real GoldSrc lightstyle ID this is; `style` here is that
    // same byte, copied so a reader walking this table alone (without also
    // cross-referencing MpakFaceInfo) still has it. `page`/`blockX`/
    // `blockY` are in the SAME atlas `MpakFaceInfo::lightmapPage` etc.
    // already describes for style 0 - X9's future consumer picks the
    // matching block for whichever style(s) are currently active and
    // blends them the way `R_BuildLightMap` does (`ref/gl/gl_rsurf.c:
    // 725-789`: `r_blocklights += samples[styleSlot] * g_lightstylevalue[
    // style]`), not a new blending rule invented here.
#pragma pack(push, 1)
    struct MpakExtraLightBlock {
        uint32_t faceIndex = 0;  // index into MpakFaceInfo / face_draw_cmds, this archive's own face order
        uint8_t styleSlot = 0;   // 1, 2, or 3 - which of MpakFaceInfo::styles[1..3] this block is for
        uint8_t style = 255;     // MpakFaceInfo::styles[styleSlot] verbatim, 255 never appears (would mean unused)
        uint8_t _pad[2] = {0, 0};
        uint32_t page = 0;       // which atlas page (same page space as MpakFaceInfo::lightmapPage)
        uint32_t blockX = 0;     // bordered block origin within that page (luxels)
        uint32_t blockY = 0;
    };
#pragma pack(pop)
    static_assert(sizeof(MpakExtraLightBlock) == 20, "MpakExtraLightBlock must stay a stable, tightly packed 20 bytes - the C reader computes count from lenExtraLightBlocks / 20");

} // namespace formats
