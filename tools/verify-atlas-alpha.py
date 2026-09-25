#!/usr/bin/env python3
"""
verify-atlas-alpha.py - texel-level check of the v10 per-texture DXT1 mip
chains' punch-through alpha (docs/r3d/fork-plan.md's XB section, "the Xbox
atlas never carried usable alpha for masked textures" / the follow-on
fixOpaqueTexelsPunchedThrough fix, TextureOpt.cpp).

Decodes every BC1/DXT1 block of every mip level of every texture in a baked
v10 .mpak's Xbox texture directory and compares each texel against the
pre-compression RGBA source that was actually fed to the encoder at that
level, flagging any texel where the source was genuinely opaque (alpha >=
128) but the compressed block decodes it as fully transparent (BC1 3-color
mode, selector 3). 128 is the encoder's own threshold, not this script's
invention: compressToDXT forces alpha < 128 to the transparent bucket
(TextureOpt.cpp:153), fixOpaqueTexelsPunchedThrough's own retry checks
alpha >= 128 (TextureOpt.cpp:84), and texture_opt_test.cpp's header comment
states the same invariant. The old v6-v8 script used alpha == 255, which
only worked because a v8 shared-atlas texture had exactly two alpha values;
v10 mip levels have real intermediate alphas from box-filtering
(downsampleRgba, PakManager.cpp:121-147), so 255 would miss real
violations there.

Filename note: there has been no shared atlas since v9 (MpakHeader.h's v8
-> v9 note - each texture is now independently hardware-WRAP addressed).
The name stays because divergences.md #99/#101 and fork-plan.md already
cite this exact path; renaming it would just be a second thing to keep in
sync.

Why this is a revival, not a rewrite from the archive alone: the encoder's
retry can decide a block is unfixable and leave it as originally encoded
(TextureOpt.cpp:65-67) - so whether the invariant holds is an empirical
property of the actual compressed bytes, not something derivable from the
.mpak by itself. Reconstructing what SHOULD have been fed to the encoder
would mean re-implementing resampleRgba's bilinear/nearest resize
(PakManager.cpp:21-60) and downsampleRgba's box filter and rounding
(PakManager.cpp:121-147) in Python - exactly the "looks equivalent" copy
this project's reference-fidelity rules warn against. So the baker dumps
the real per-level buffer it is about to compress, and this script only
does addressing and BC1 decoding.

Usage:
    tools/verify-atlas-alpha.py <mpak_path> <rgba_dump_path>

<rgba_dump_path> is produced by building tools/asset-studio's AssetStudio
and baking with the R3D_DUMP_TEXTURE_RGBA environment variable set to a
file path (successor to the pre-v9 R3D_DUMP_ATLAS_RGBA hook, removed in
1604d2e7 when the shared atlas went away) - e.g.:

    R3D_DUMP_TEXTURE_RGBA=/tmp/dump.rgba \\
      AssetStudio.exe --game-dir <retail HL install> --map maps/<name>.bsp \\
      --output /tmp/<name>.mpak --target xbox --profile nv2a
    tools/verify-atlas-alpha.py /tmp/<name>.mpak /tmp/dump.rgba

Exit codes:
    0 - every texel in the archive was checked, and none mismatched
        (texels checked is always > 0 for exit 0; checking zero texels is
        exit 2, never a silent pass).
    1 - at least one opaque-source-but-transparent-decoded mismatch.
    2 - usage error, or a structural problem with the archive or the dump
        (wrong version, a field list that no longer matches the on-disk
        header, a section walk that doesn't land on the file's own size, a
        dump that doesn't match the archive's own texture directory, a
        dump record's digest that doesn't match the archive's compressed
        bytes at that texture/level, ...).
        Anything that means this script could not really check the bytes
        is exit 2, not a silent pass or a false mismatch.

Dump/archive binding: each dump record (v2) carries a CRC-32 of the exact
compressed DXT bytes buildDxt1MipChain produced for that level, alongside the
pre-compression RGBA source. This script recomputes the same CRC-32 over the
archive's own bytes at that texture/level and requires a match before
decoding - otherwise a stale dump from an earlier or different bake, with
matching texture dimensions, could be paired with today's archive and a real
encoder regression would silently read as exit 0 (codex-review finding,
X9-V10-tooling QA round 1). The CRC-32 is ISO 3309 / IEEE 802.3, the same
algorithm zlib.crc32 implements (checked against the standard "123456789" ->
0xCBF43926 test vector) - not this script's own invention, and not a
cryptographic integrity check, just a cheap way to tell "these bytes" apart
from "some other bytes of the same size".
"""
import struct
import sys
import zlib

# --- format constants, looked up rather than guessed (AGENTS.md) ---

MPAK_BSP_VERSION = 30          # HLBSP_VERSION, common/bspfile.h; MpakHeader.h:38
MPAK_DESCRIPTOR_OFFSET = 128   # MpakHeader.h:37 MPAK_DESCRIPTOR_OFFSET
# MpakHeader6::version's default is the MAP format version, kept in lockstep
# with filesystem/mpak_layout.h:54's MPAK_LAYOUT_VERSION - MpakHeader.h:213.
MPAK_LAYOUT_VERSION = 12
MPAK_FLAG_XBOX = 1        # MpakHeader.h:18
MPAK_FLAG_NV2A_BLOCK = 2  # MpakHeader.h:19
SECTOR = 2048
VERTEX_SIZE = 48    # formats::Vertex, PakManager.h:13-27 (post-v9, rect[4] removed)
INDEX_SIZE = 2       # uint16_t
TEXTURE_INFO_SIZE = 20   # MpakTextureInfo, MpakHeader.h:285-300
ALIGN = 128           # v10 per-texture chain padding, MpakHeader.h:84-93 / PakManager.cpp:448-468
DXT1_BLOCK_SIZE = 8
DUMP_MAGIC = b"RTXD"
DUMP_VERSION = 2  # v2 adds a per-record CRC-32 binding the source to its compressed bytes
DUMP_RECORD_HEADER_FMT = "<5I"  # texture, level, width, height, crc32-of-compressed-bytes
DUMP_RECORD_HEADER_SIZE = struct.calcsize(DUMP_RECORD_HEADER_FMT)
ALPHA_OPAQUE_THRESHOLD = 128  # TextureOpt.cpp:84, :153; texture_opt_test.cpp
MAX_RETAINED_MISMATCHES = 20  # only this many Mismatch objects are ever kept; see Result

# MpakHeader6's own field order (MpakHeader.h:211-275) - every field after
# the 4-byte magic is a uint32_t, byte order IS the format (no offset
# table), so this list must track that struct exactly. 44 fields (v11).
_HEADER_FIELDS = [
    "version", "headerSize", "bakerBuildId", "flags",
    "vCount", "iCount",
    "lenVisibility", "lenNodes", "lenLeaves", "lenMarksurfaces", "lenFaceDrawCmds",
    "lenPlanes", "lenClipnodes", "lenModels",
    "textureCount", "lenTextureDir", "lenTextureData", "batchCount", "lenBatches", "lenFaceRemap",
    "count64", "lenArray64", "count256", "lenArray256", "count512", "lenArray512",
    "lenAssetDict", "lenAssetData",
    "lenEntities",
    "lightmapWidth", "lightmapHeight", "lightmapLayers", "lightmapFormat", "lenLightmapAtlas",
    "lenFaceInfo", "lenTexinfo",
    "lenVertexes", "lenEdges", "lenSurfedges", "lenFaces", "lenLighting", "lenMiptex",
    "lenExtraLightBlocks",
    "lenFaceVertexBase",  # v11: per-face vertex-chunk bases, multi-chunk maps only
]
assert len(_HEADER_FIELDS) == 44


class VerifyError(Exception):
    """Any structural problem with the archive or the dump - always exit 2."""


class Mismatch:
    __slots__ = ("texture", "level", "x", "y", "src_rgba", "c0", "c1", "selectors")

    def __init__(self, texture, level, x, y, src_rgba, c0, c1, selectors):
        self.texture = texture
        self.level = level
        self.x = x
        self.y = y
        self.src_rgba = src_rgba
        self.c0 = c0
        self.c1 = c1
        self.selectors = selectors

    def __repr__(self):
        return (f"texture={self.texture} level={self.level} x={self.x} y={self.y} "
                f"src_rgba={self.src_rgba} c0={self.c0:#06x} c1={self.c1:#06x} "
                f"selectors={self.selectors:#010x}")


class Result:
    """mismatch_count is the true total and is what the exit code and the
    printed summary are based on. mismatches holds at most
    MAX_RETAINED_MISMATCHES Mismatch objects, purely for the detail report -
    codex-review flagged that retaining one object per mismatched texel could
    OOM the process on a genuinely broken encoder over a corpus the size this
    topic's own verification measured (164M+ texels), when only the first 20
    are ever printed (X9-V10-tooling QA round 1)."""
    def __init__(self):
        self.textures = 0
        self.levels = 0
        self.texels = 0
        self.mismatch_count = 0
        self.mismatches = []
        self.informational = 0  # source alpha < 128, decoded opaque - not a failure


def _align128(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def _sector_pad(n):
    return (n + SECTOR - 1) // SECTOR * SECTOR


def _parse_header(data):
    if len(data) < MPAK_DESCRIPTOR_OFFSET + 4:
        raise VerifyError("file is smaller than the descriptor offset")

    (bsp_version,) = struct.unpack_from("<i", data, 0)
    if bsp_version != MPAK_BSP_VERSION:
        raise VerifyError(f"dheader_t version at offset 0 is {bsp_version}, not {MPAK_BSP_VERSION}")

    magic = data[MPAK_DESCRIPTOR_OFFSET:MPAK_DESCRIPTOR_OFFSET + 4]
    if magic != b"MPAK":
        raise VerifyError(f"no MPAK descriptor at offset {MPAK_DESCRIPTOR_OFFSET} (found {magic!r})")

    off = MPAK_DESCRIPTOR_OFFSET + 4
    h = {}
    for name in _HEADER_FIELDS:
        if off + 4 > len(data):
            raise VerifyError(f"file truncated while reading header field {name!r}")
        h[name] = struct.unpack_from("<I", data, off)[0]
        off += 4

    if h["version"] != MPAK_LAYOUT_VERSION:
        raise VerifyError(
            f"descriptor version is {h['version']}, this tool only understands "
            f"MPAK_LAYOUT_VERSION {MPAK_LAYOUT_VERSION} (MpakHeader.h:213, mpak_layout.h:54) - "
            "a future format bump must fail loudly here, not be misparsed with a stale field list"
        )

    expected_header_size = 4 + 4 * len(_HEADER_FIELDS)
    if h["headerSize"] != expected_header_size:
        raise VerifyError(
            f"descriptor headerSize is {h['headerSize']}, expected {expected_header_size} - "
            "this script's own field list no longer matches the on-disk header"
        )

    if (h["flags"] & MPAK_FLAG_XBOX) == 0 or (h["flags"] & MPAK_FLAG_NV2A_BLOCK) == 0:
        raise VerifyError(
            f"descriptor flags {h['flags']:#x} must have both MPAK_FLAG_XBOX (1) and "
            "MPAK_FLAG_NV2A_BLOCK (2) set - this tool only checks baked Xbox nv2a archives"
        )

    return h


def _walk_sections(h, file_size):
    """Re-derive every section's offset from the descriptor, in write order -
    PakManager::save()'s own writePadded() call sequence (PakManager.cpp:504-563),
    which PakManager::verifyFile() re-walks the same way (PakManager.cpp:737-782).
    Since this tool requires MPAK_FLAG_XBOX (see _parse_header), the Xbox-only
    texture directory/data/batches are always present and the PC-only texture
    arrays are always absent, matching save()'s own `if (mIsXboxTarget)` gating.
    """
    cursor = SECTOR
    offsets = {}

    def place(name, length):
        nonlocal cursor
        offsets[name] = cursor
        if length > 0:
            cursor += _sector_pad(length)

    # v12: every classic BSP lump first, then the renderer sections.
    place("visibility", h["lenVisibility"])
    place("nodes", h["lenNodes"])
    place("leaves", h["lenLeaves"])
    place("marksurfaces", h["lenMarksurfaces"])
    place("planes", h["lenPlanes"])
    place("clipnodes", h["lenClipnodes"])
    place("models", h["lenModels"])
    place("entities", h["lenEntities"])
    place("texinfo", h["lenTexinfo"])
    place("vertexesRaw", h["lenVertexes"])
    place("edges", h["lenEdges"])
    place("surfedges", h["lenSurfedges"])
    place("faces", h["lenFaces"])
    place("lighting", h["lenLighting"])
    place("miptex", h["lenMiptex"])
    place("vertices", h["vCount"] * VERTEX_SIZE)
    place("indices", h["iCount"] * INDEX_SIZE)
    place("faceDrawCmds", h["lenFaceDrawCmds"])
    place("textureDir", h["lenTextureDir"])
    place("textureData", h["lenTextureData"])
    place("batches", h["lenBatches"])
    place("faceRemap", h["lenFaceRemap"])
    place("assetDict", h["lenAssetDict"])
    place("assetData", h["lenAssetData"])
    place("lightmapAtlas", h["lenLightmapAtlas"])
    place("faceInfo", h["lenFaceInfo"])
    place("extraLightBlocks", h["lenExtraLightBlocks"])
    place("faceVertexBase", h["lenFaceVertexBase"])

    if cursor != file_size:
        raise VerifyError(
            f"descriptor sections end at {cursor} but the file is {file_size} bytes - "
            "a stale field list would shift everything after the point it diverges"
        )

    return offsets


def _level_dims(width, height, level):
    """(pw, ph): the padded dimensions buildDxt1MipChain compresses at this
    level - PakManager.cpp:161 (`pw = max(lw, 4)`), a level under 4 texels on
    a side still takes a whole 4x4 block."""
    lw = max(width >> level, 1)
    lh = max(height >> level, 1)
    return max(lw, 4), max(lh, 4)


def _chain_levels(width, height):
    return max(width.bit_length() - 1, height.bit_length() - 1) + 1


def _chain_length(width, height):
    total = 0
    for level in range(_chain_levels(width, height)):
        pw, ph = _level_dims(width, height, level)
        total += (pw // 4) * (ph // 4) * DXT1_BLOCK_SIZE
    return total


def _parse_texture_dir(data, h, offsets):
    if h["textureCount"] == 0:
        raise VerifyError("textureCount is 0 - nothing to check is exit 2, never a silent pass")
    if h["lenTextureDir"] != h["textureCount"] * TEXTURE_INFO_SIZE:
        raise VerifyError(
            f"lenTextureDir {h['lenTextureDir']} != textureCount {h['textureCount']} * "
            f"sizeof(MpakTextureInfo) ({TEXTURE_INFO_SIZE})"
        )

    dir_off = offsets["textureDir"]
    dir_bytes = data[dir_off:dir_off + h["lenTextureDir"]]
    if len(dir_bytes) != h["lenTextureDir"]:
        raise VerifyError("file truncated inside the texture directory")

    entries = []
    running = 0
    for i, (width, height, log2w, log2h, data_length) in enumerate(
        struct.iter_unpack("<5I", dir_bytes)
    ):
        if width != (1 << log2w):
            raise VerifyError(f"texture {i}: width {width} != 1<<log2Width ({1 << log2w})")
        if height != (1 << log2h):
            raise VerifyError(f"texture {i}: height {height} != 1<<log2Height ({1 << log2h})")

        expected_length = _chain_length(width, height)
        if data_length != expected_length:
            raise VerifyError(
                f"texture {i}: dataLength {data_length} != computed chain length {expected_length} "
                f"for {width}x{height}"
            )

        offset = running
        running += _align128(data_length)
        entries.append({
            "width": width, "height": height,
            "log2Width": log2w, "log2Height": log2h,
            "dataLength": data_length, "offset": offset,
            "levels": _chain_levels(width, height),
        })

    if running != h["lenTextureData"]:
        raise VerifyError(
            f"sum of align128(dataLength) over the texture directory is {running}, "
            f"but lenTextureData is {h['lenTextureData']}"
        )

    return entries


def _parse_dump(dump_bytes, entries):
    if len(dump_bytes) < 8:
        raise VerifyError(
            f"dump is {len(dump_bytes)} bytes, smaller than the 8-byte header "
            f"({DUMP_MAGIC!r} + u32 version) - too short to be a real dump"
        )
    if dump_bytes[:4] != DUMP_MAGIC:
        raise VerifyError(f"dump does not start with {DUMP_MAGIC!r}")
    (version,) = struct.unpack_from("<I", dump_bytes, 4)
    if version != DUMP_VERSION:
        raise VerifyError(
            f"dump format version is {version}, expected {DUMP_VERSION} - v2 added a per-record "
            "CRC-32 binding the source to the exact compressed bytes it was measured against "
            "(see this script's module doc); a v1 dump has no digest to check and must not be "
            "silently accepted"
        )

    off = 8
    records = {}  # (texture, level) -> (rgba bytes, expected crc32 of the compressed mip)
    for t, entry in enumerate(entries):
        for level in range(entry["levels"]):
            if off + DUMP_RECORD_HEADER_SIZE > len(dump_bytes):
                raise VerifyError(f"dump truncated before the record for texture {t} level {level}")
            tex_idx, rec_level, width, height, digest = struct.unpack_from(DUMP_RECORD_HEADER_FMT, dump_bytes, off)
            off += DUMP_RECORD_HEADER_SIZE
            if tex_idx != t or rec_level != level:
                raise VerifyError(
                    f"dump record order mismatch: expected (texture={t}, level={level}), "
                    f"found (texture={tex_idx}, level={rec_level}) - records must appear in "
                    "(texture 0..N-1, level 0..L-1) order"
                )
            expected_w, expected_h = _level_dims(entry["width"], entry["height"], level)
            if width != expected_w or height != expected_h:
                raise VerifyError(
                    f"dump record for texture {t} level {level}: dims {width}x{height} != "
                    f"expected {expected_w}x{expected_h}"
                )
            payload_len = width * height * 4
            if off + payload_len > len(dump_bytes):
                raise VerifyError(f"dump truncated inside the RGBA payload for texture {t} level {level}")
            records[(t, level)] = (dump_bytes[off:off + payload_len], digest)
            off += payload_len

    if off != len(dump_bytes):
        raise VerifyError(f"dump has {len(dump_bytes) - off} trailing bytes after the last record")

    return records


def _decode_level(archive_bytes, level_off, src_rgba, pw, ph, texture_idx, level, result):
    blocks_x, blocks_y = pw // 4, ph // 4
    for by in range(blocks_y):
        for bx in range(blocks_x):
            block_off = level_off + (by * blocks_x + bx) * DXT1_BLOCK_SIZE
            block = archive_bytes[block_off:block_off + DXT1_BLOCK_SIZE]
            c0, c1 = struct.unpack_from("<HH", block, 0)
            selectors = struct.unpack_from("<I", block, 4)[0]

            result.texels += 16
            if c0 > c1:
                # 4-color mode: nothing can decode as transparent, so there is
                # nothing to check for these 16 texels.
                continue

            for py in range(4):
                for px in range(4):
                    idx = py * 4 + px
                    sel = (selectors >> (idx * 2)) & 0x3
                    decoded_transparent = (sel == 3)
                    sx, sy = bx * 4 + px, by * 4 + py
                    so = (sy * pw + sx) * 4
                    sr, sg, sb, sa = src_rgba[so], src_rgba[so + 1], src_rgba[so + 2], src_rgba[so + 3]
                    if sa >= ALPHA_OPAQUE_THRESHOLD and decoded_transparent:
                        result.mismatch_count += 1
                        if len(result.mismatches) < MAX_RETAINED_MISMATCHES:
                            result.mismatches.append(Mismatch(
                                texture_idx, level, sx, sy, (sr, sg, sb, sa), c0, c1, selectors
                            ))
                    elif sa < ALPHA_OPAQUE_THRESHOLD and not decoded_transparent:
                        result.informational += 1


def verify(mpak_bytes, dump_bytes):
    """Verify one baked v10 Xbox nv2a .mpak's DXT1 mip chains against the
    pre-compression RGBA source dumped alongside it. Returns a Result.
    Raises VerifyError for anything that means the bytes could not really
    be checked (exit 2 territory) - never returns a false pass."""
    h = _parse_header(mpak_bytes)
    offsets = _walk_sections(h, len(mpak_bytes))
    entries = _parse_texture_dir(mpak_bytes, h, offsets)
    records = _parse_dump(dump_bytes, entries)

    data_base = offsets["textureData"]
    result = Result()
    result.textures = len(entries)

    for t, entry in enumerate(entries):
        chain_off = data_base + entry["offset"]
        level_off = chain_off
        for level in range(entry["levels"]):
            pw, ph = _level_dims(entry["width"], entry["height"], level)
            src_rgba, expected_digest = records[(t, level)]
            level_bytes = (pw // 4) * (ph // 4) * DXT1_BLOCK_SIZE
            actual_digest = zlib.crc32(mpak_bytes[level_off:level_off + level_bytes]) & 0xFFFFFFFF
            if actual_digest != expected_digest:
                raise VerifyError(
                    f"texture {t} level {level}: dump digest {expected_digest:#010x} does not "
                    f"match the archive's compressed bytes ({actual_digest:#010x}) - this dump "
                    "was not taken from this exact bake (stale or cross-archive dump), so it "
                    "cannot be trusted to verify these bytes"
                )
            _decode_level(mpak_bytes, level_off, src_rgba, pw, ph, t, level, result)
            level_off += level_bytes
            result.levels += 1

    if result.texels == 0:
        raise VerifyError("0 texels checked - treated as a failure to verify, never a silent pass")

    return result


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2

    mpak_path, dump_path = argv[1], argv[2]
    try:
        with open(mpak_path, "rb") as f:
            mpak_bytes = f.read()
    except OSError as e:
        print(f"error: cannot read {mpak_path}: {e}")
        return 2
    try:
        with open(dump_path, "rb") as f:
            dump_bytes = f.read()
    except OSError as e:
        print(f"error: cannot read {dump_path}: {e}")
        return 2

    try:
        result = verify(mpak_bytes, dump_bytes)
    except VerifyError as e:
        print(f"error: {e}")
        return 2

    print(f"Textures checked: {result.textures}")
    print(f"Mip levels checked: {result.levels}")
    print(f"Texels checked: {result.texels}")
    print(f"Opaque-source-but-transparent-decoded mismatches: {result.mismatch_count}")
    print(f"Informational (transparent-source-but-opaque-decoded): {result.informational}")
    for m in result.mismatches[:MAX_RETAINED_MISMATCHES]:
        print(" ", m)
    if result.mismatch_count > len(result.mismatches):
        print(f"  ... {result.mismatch_count - len(result.mismatches)} more mismatch(es) not retained "
              f"(only the first {MAX_RETAINED_MISMATCHES} are kept, to bound memory on a widespread failure)")

    return 1 if result.mismatch_count else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
