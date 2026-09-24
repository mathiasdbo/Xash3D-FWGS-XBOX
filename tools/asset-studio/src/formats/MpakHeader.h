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

    // Numbers ONLY UiPacker::packMenuBackground's own archive (menu_bg.mpak):
    // descriptor at offset 0, no dheader_t, sections are a 24-byte meta block
    // then one DXT blob - a completely different, older, simpler layout than
    // the map format below, not a map archive that happens to be small.
    // Independent of MpakHeader6::version (the map format version, kept in
    // lockstep with filesystem/mpak_layout.h's MPAK_LAYOUT_VERSION): before
    // this constant existed, packMenuBackground left `version` at its
    // MpakHeader6 default and so silently inherited every map-format bump
    // (X9-V10 found it shipping a deployed archive stamped 6 when the struct
    // it actually wrote had already moved past that shape entirely). Bump
    // this when packMenuBackground's OWN layout changes, or when a map-format
    // change moves any MpakHeader6 field that function writes (see its own
    // static_asserts in UiPacker.cpp, which pin those offsets and say the
    // same thing).
    //
    // The archive KIND is identified by the descriptor's position (offset 0,
    // no BSP30 dheader_t) - not by this number. Value 1 numerically matches
    // the historic pre-v7 map format (back when map descriptors also sat at
    // offset 0), and that overlap is harmless: every map reader requires a
    // real dheader_t plus `version == MPAK_LAYOUT_VERSION` at offset 128
    // (filesystem/mpak.c:136-160, mpak_layout.c:70), so nothing that reads a
    // map archive can mistake this for one, regardless of the number.
    //
    // MPAK_BAKER_BUILD_ID above stays the shared, baker-wide build stamp
    // (any rebake with unchanged `version` but different bytes) - it is not
    // a layout version and this constant doesn't replace it.
    constexpr uint32_t MPAK_UI_BACKGROUND_VERSION = 1;

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
    // MPAK_DESCRIPTOR_OFFSET (see the v7 first-sector note above; this is
    // PakManager::save(), and also UiPacker::packMenuAssets/menu.mpak, which
    // goes through PakManager::save() and so IS a map-shaped archive, dheader_t
    // and all). UiPacker::packMenuBackground/menu_bg.mpak is the one archive
    // that still writes this same struct at offset 0, with no dheader_t in
    // front of it - it is not a map and no engine path reads it yet, and it
    // has its own version number (MPAK_UI_BACKGROUND_VERSION above), not this
    // struct's `version` default. Corrected 2026-09 (X9-V10): earlier text
    // here said packMenuBackground was the general case for "UiPacker's menu
    // archives" - wrong, packMenuAssets is map-shaped and packMenuBackground
    // is the one exception, not the other way round.
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
    // v9 -> v10: every Xbox texture carries its full DXT1 mip chain, base
    // level down to 1x1 (max(log2Width, log2Height) + 1 levels), levels
    // back to back; a level under 4 texels on a side still takes whole 4x4
    // blocks. MpakTextureInfo keeps its layout - dataLength is now the
    // chain's length - and each texture's data is zero-padded to the next
    // 128 bytes inside the textureData lump, so a texture's offset is the
    // running sum of align128(dataLength) over prior entries. Before v10
    // every texture had one level, and distant tiled faces aliased into
    // moire (divergences.md #101 (g)). A v9 reader would take the chain for
    // a wrong-sized single level, hence the bump.
    //
    // v8 -> v9: the Xbox diffuse-texture pipeline stops sharing one 2048x2048
    // DXT1 atlas per map. The atlas's own per-vertex placement remap
    // (`Vertex::rect`, normalized ATLAS-space [x,y,w,h]) could only represent
    // ONE texture repeat per face - `rect[0] + frac(u)*rect[2]` collapses
    // every repeat past the first back onto the same fractional value, which
    // is wrong on any tiling floor/wall, and subdividing tiled faces at tile
    // boundaries to work around it blows the u16 vertex-index limit on large
    // outdoor maps (crossfire's worst face alone needs ~22,560 tiles).
    // Replaced with NV2A's native hardware WRAP texture addressing (needs no
    // atlas and no remap - the GPU tiles for free), which means each source
    // texture must stand alone as its OWN independently-sized, independently
    // POT-resampled image (hardware WRAP + BASE_SIZE_U/V sizing both require
    // this - X9-J's own bring-up already established BASE_SIZE_U/V as
    // independent per-axis log2 fields, divergences.md #100).
    // `atlasWidth`/`atlasHeight`/`lenAtlas` are GONE; in their place,
    // `textureCount`/`lenTextureDir`/`lenTextureData` describe a directory of
    // per-texture `MpakTextureInfo` records plus their concatenated DXT1
    // blobs (still linear/unswizzled, same as the atlas always shipped -
    // swizzling stays UiPacker's menu-background-only trick,
    // `TextureOpt::swizzleNV2A`'s one real call site), and
    // `batchCount`/`lenBatches` describe the `MpakTextureBatch` list a
    // per-texture draw path needs to group faces by texture. `Vertex::rect[4]`
    // (the now-dead atlas remap) is removed too - 64 bytes -> 48.
    // Since texture is now the PRIMARY sort key for draw batching,
    // BspParser.cpp reorders MpakFaceInfo/vertices/indices/FaceDrawCmd by
    // (textureIndex, PVS leaf) instead of leaving them in raw LUMP_FACES
    // order - `lenFaceRemap` is the resulting originalToSortedFaceIndex[]
    // inverse permutation, one uint32_t per ORIGINAL-order face. This table is
    // REQUIRED reading for anyone indexing this archive's faces with an index
    // that came from the classic BSP lumps (LUMP_LEAFS/LUMP_MARKSURFACES, or
    // the engine's own runtime msurface_t array) rather than from this
    // archive's own sorted order - ref/nv2a's nv2a_mark_leaves() is exactly
    // this case (it derives a face index from `surf - world->surfaces`, a
    // classic-lump-order index) and must translate every such index through
    // this table before touching anything sized by the sorted face count.
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
        // Default is the MAP format version, kept in lockstep with
        // filesystem/mpak_layout.h's MPAK_LAYOUT_VERSION. PakManager::save()
        // (map archives, and UiPacker::packMenuAssets/menu.mpak, which
        // writes through it) both use this default unmodified.
        // UiPacker::packMenuBackground/menu_bg.mpak is the one writer that
        // overrides this field with MPAK_UI_BACKGROUND_VERSION instead - see
        // that constant's own doc comment above for why it needs a number
        // that isn't this one.
        uint32_t version = 10;
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

        // --- MPAKv9 additions: per-texture Xbox draws (see the v8 -> v9 note
        // above) replace the old shared-atlas fields (atlasWidth/atlasHeight/
        // lenAtlas, removed) in this exact position. ---
        uint32_t textureCount = 0;   // unique per-texture DXT1 images this map actually uses
        uint32_t lenTextureDir = 0;  // textureCount * sizeof(MpakTextureInfo)
        uint32_t lenTextureData = 0; // total bytes of all per-texture DXT1 mip chains in directory order, each zero-padded to 128 bytes (v10)
        uint32_t batchCount = 0;     // texture-draw batches; == textureCount architecturally (see MpakTextureBatch's own doc comment), kept explicit for robustness/clarity
        uint32_t lenBatches = 0;     // batchCount * sizeof(MpakTextureBatch)
        uint32_t lenFaceRemap = 0;   // (original BSP face count) * sizeof(uint32_t) - originalToSortedFaceIndex[]; the original face count is lenFaceRemap / sizeof(uint32_t)

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

    // v9: one entry per unique texture this map actually uses - the Xbox
    // per-texture-WRAP replacement for the old shared atlas (see MpakHeader6's
    // v8 -> v9 note above). Directory order IS the write order of the
    // `lenTextureData` blob: this entry's own byte offset into that blob is
    // the running sum of `align128(dataLength)` over every PRIOR directory
    // entry (v10: each entry is zero-padded to the next 128 bytes, Xbox D3D's
    // own texture base alignment - PakManager.cpp's save()/buildDxt1MipChain;
    // corrected 2026-09, this used to say plain `dataLength`, true only before
    // v10's mip chains made per-entry padding necessary) - no stored offset
    // field, matching every other section-length-only lump this format
    // already uses (see the "no offset table" note above).
#pragma pack(push, 1)
    struct MpakTextureInfo {
        uint32_t width;      // POT, rounded UP (never down - see
                              // nearestPowerOfTwo's own comment,
                              // PakManager.cpp) INDEPENDENTLY per axis from
                              // the source's native size (NV2A's
                              // BASE_SIZE_U/BASE_SIZE_V are independent log2
                              // fields - textures need not be square; X9-J's
                              // own bring-up already established this,
                              // divergences.md #100)
        uint32_t height;     // POT, rounded up independently per axis
        uint32_t log2Width;  // log2(width) - the BASE_SIZE_U value
        uint32_t log2Height; // log2(height) - the BASE_SIZE_V value
        uint32_t dataLength; // v10: this texture's whole DXT1 mip chain, in bytes (padding to 128 not included)
    };
#pragma pack(pop)
    static_assert(sizeof(MpakTextureInfo) == 20, "MpakTextureInfo must stay a stable, tightly packed 20 bytes - the C reader computes textureCount from lenTextureDir / 20");

    // v9: one entry per contiguous run of same-texture faces in the SORTED
    // face order (see MpakHeader6's v8 -> v9 note above) - a per-texture
    // hardware-WRAP draw path needs exactly this to bind one texture and
    // issue one draw call per batch. `batchCount` <= `textureCount`, NOT
    // ==: texture ID is the sort's PRIMARY key, so every texture that IS
    // used by at least one face lands in exactly one contiguous run - but
    // `textureDir` (built from every miptex the map references at all,
    // BspParser.cpp) can contain entries no face's texinfo ever actually
    // uses, and those get no batch (real, measured: c1a0 164 textures / 129
    // batches, crossfire 120/91 - 15-18% of baked texture-data bytes on
    // both are for entries with zero draw calls, codex-review finding).
    // `batchCount` is still its own explicit field, matching this format's
    // general preference for explicit lengths over inferred ones - just
    // don't assume it equals `textureCount`.
#pragma pack(push, 1)
    struct MpakTextureBatch {
        uint32_t textureIndex; // index into the texture directory (MpakTextureInfo[])
        uint32_t startFace;    // first face, in SORTED/archive order, belonging to this batch
        uint32_t faceCount;    // number of faces (sorted order) in this batch - contiguous by construction
        uint32_t startIndex;   // first index in the shared index buffer for this batch (== FaceDrawCmd[startFace].start_index)
        uint32_t indexCount;   // total indices for this batch (== sum of FaceDrawCmd[startFace..startFace+faceCount).index_count, contiguous by construction)
    };
#pragma pack(pop)
    static_assert(sizeof(MpakTextureBatch) == 20, "MpakTextureBatch must stay a stable, tightly packed 20 bytes - the C reader computes batchCount from lenBatches / 20");

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
