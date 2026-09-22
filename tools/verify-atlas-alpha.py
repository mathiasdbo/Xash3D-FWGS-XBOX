#!/usr/bin/env python3
"""
verify-atlas-alpha.py - texel-level regression check for the Xbox atlas's
punch-through alpha (docs/r3d/fork-plan.md's XB section, "the Xbox atlas
never carried usable alpha for masked textures" / the follow-on
fixOpaqueTexelsPunchedThrough fix).

Decodes every BC1/DXT1 block in a baked .mpak's Xbox texture atlas and
compares each texel against the pre-compression RGBA source, flagging any
texel where the source was genuinely opaque (alpha == 255) but the
compressed atlas decodes it as fully transparent (BC1 selector 3). That
mismatch is the exact defect class asset-pipeline-specialist review found
in TextureOpt.cpp's punch-through path, and the one codex-review round 2
refused to accept as an undocumented residual ("the observed percentage
does not bound other maps") - this script is what closes that objection
for real, on any map, not just the one it was first measured against.

Usage:
    tools/verify-atlas-alpha.py <mpak_path> <rgba_dump_path>

<rgba_dump_path> is the pre-compression atlas RGBA buffer, produced by
building tools/asset-studio's AssetStudio.exe and baking with the
R3D_DUMP_ATLAS_RGBA environment variable set to a file path
(PakManager.cpp's save() writes the raw mAtlasRgba buffer there,
unconditionally cheap unless the variable is set) - e.g.:

    R3D_DUMP_ATLAS_RGBA=/tmp/atlas.rgba \\
      AssetStudio.exe --game-dir <retail HL install> --map maps/<name>.bsp \\
      --output /tmp/<name>.mpak --target xbox --profile nv2a
    tools/verify-atlas-alpha.py /tmp/<name>.mpak /tmp/atlas.rgba

Exits 0 and prints "0 mismatches" on success, exits 1 otherwise - suitable
for a shell loop over a retail map corpus, matching this project's other
tools/test_*.py harnesses.
"""
import struct
import sys

# mpak_header_t's own field order (filesystem/mpak_layout.h) - every field
# after the 4-byte magic is a uint32_t, and byte order IS the format (no
# offset table), so this list must track that header exactly.
_HEADER_FIELDS = [
    "version", "headerSize", "bakerBuildId", "flags",
    "vCount", "iCount",
    "lenVisibility", "lenNodes", "lenLeaves", "lenMarksurfaces", "lenFaceDrawCmds",
    "lenPlanes", "lenClipnodes", "lenModels",
    "atlasWidth", "atlasHeight", "lenAtlas",
    "count64", "lenArray64", "count256", "lenArray256", "count512", "lenArray512",
    "lenAssetDict", "lenAssetData",
    "lenEntities",
    "lightmapWidth", "lightmapHeight", "lightmapLayers", "lightmapFormat", "lenLightmapAtlas",
    "lenFaceInfo", "lenTexinfo",
    "lenVertexes", "lenEdges", "lenSurfedges", "lenFaces", "lenLighting", "lenMiptex",
    "lenExtraLightBlocks",
]
_SECTOR = 2048
_DESCRIPTOR_OFFSET = 128


def _parse_header(data):
    off = _DESCRIPTOR_OFFSET + 4  # skip the descriptor's own 4-byte magic
    h = {}
    for name in _HEADER_FIELDS:
        h[name] = struct.unpack_from("<I", data, off)[0]
        off += 4
    return h


def _sector_pad(n):
    return ((n + _SECTOR - 1) // _SECTOR) * _SECTOR


def _locate_atlas(h):
    # Sections are laid out sequentially from sector 1, sector-padded, in
    # the exact order save() writes them - this must track PakManager.cpp's
    # own writePadded() call sequence for the atlas offset to land right.
    cursor = _SECTOR

    def place(length):
        nonlocal cursor
        o = cursor
        if length > 0:
            cursor += _sector_pad(length)
        return o

    place(h["vCount"] * 64)
    place(h["iCount"] * 2)
    for key in ("lenVisibility", "lenNodes", "lenLeaves", "lenMarksurfaces",
                "lenFaceDrawCmds", "lenPlanes", "lenClipnodes", "lenModels"):
        place(h[key])
    return place(h["lenAtlas"])


def _rgb565_to_888(c):
    r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def verify(mpak_path, rgba_path):
    with open(mpak_path, "rb") as f:
        data = f.read()
    with open(rgba_path, "rb") as f:
        src = f.read()

    h = _parse_header(data)
    atlas_off = _locate_atlas(h)
    w, ht = h["atlasWidth"], h["atlasHeight"]
    if len(src) != w * ht * 4:
        raise ValueError(f"RGBA dump size {len(src)} doesn't match atlas {w}x{ht}x4")

    atlas = data[atlas_off:atlas_off + h["lenAtlas"]]
    blocks_x, blocks_y = w // 4, ht // 4

    mismatches = []
    total_texels = 0

    for by in range(blocks_y):
        for bx in range(blocks_x):
            block = atlas[(by * blocks_x + bx) * 8: (by * blocks_x + bx) * 8 + 8]
            c0, c1 = struct.unpack_from("<HH", block, 0)
            sel = struct.unpack_from("<I", block, 4)[0]
            r0, g0, b0 = _rgb565_to_888(c0)
            r1, g1, b1 = _rgb565_to_888(c1)
            three_color = c0 <= c1
            pal = [(r0, g0, b0, 255), (r1, g1, b1, 255), None, None]
            if three_color:
                pal[2] = ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255)
                pal[3] = (0, 0, 0, 0)  # BC1's transparent selector
            else:
                pal[2] = ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255)
                pal[3] = ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)

            for py in range(4):
                for px in range(4):
                    texel_idx = py * 4 + px
                    s = (sel >> (texel_idx * 2)) & 0x3
                    dr, dg, db, da = pal[s]
                    src_x, src_y = bx * 4 + px, by * 4 + py
                    src_off = (src_y * w + src_x) * 4
                    sr, sg, sb, sa = src[src_off], src[src_off + 1], src[src_off + 2], src[src_off + 3]
                    total_texels += 1
                    if sa == 255 and da == 0:
                        mismatches.append((src_x, src_y, (sr, sg, sb, sa), (dr, dg, db, da)))

    return total_texels, mismatches


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    total, mismatches = verify(sys.argv[1], sys.argv[2])
    print(f"Total texels checked: {total}")
    print(f"Opaque-source-but-transparent-decoded mismatches: {len(mismatches)}")
    for m in mismatches[:20]:
        print(" ", m)
    return 0 if not mismatches else 1


if __name__ == "__main__":
    sys.exit(main())
