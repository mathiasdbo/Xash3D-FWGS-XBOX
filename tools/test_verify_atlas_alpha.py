"""
Failure-path tests for tools/verify-atlas-alpha.py (v10 revival).

Run:  python -m unittest tools/test_verify_atlas_alpha.py

Per docs/r3d/lessons.md ("NEVER trust a new tool's exit code without testing
its failure path too"), every structural defect this tool claims to catch
gets a synthetic archive that actually contains it, not just a happy-path
archive that happens to pass. Archives here are built entirely in memory -
a real bake is exercised separately (see the loop log's byte-identity and
corpus-sweep verification), this file only pins the parser/decoder's own
logic: header field list, the section walk, the texture directory's
align128 addressing, the dump's record order, and the BC1 decode itself.

Cases (h) and (i) were added on QA round 1's codex-review findings: (h)
proves a dump built from a different compressed chain (matching dimensions,
different bytes) is rejected rather than silently accepted; (i) proves the
mismatch count and exit code stay correct once the number of mismatches is
well beyond MAX_RETAINED_MISMATCHES, the cap that keeps a widely-broken
encoder run from retaining one object per mismatched texel.

The two test_g_dump_header_* cases were added on QA round 2's codex-review
finding: a dump truncated to 4-7 bytes (magic present, version field absent
or partial) reached an unguarded struct.unpack_from and raised an uncaught
struct.error - Python's default exit code 1, colliding with the exit code
this tool reserves for "a real alpha mismatch was found" - instead of the
documented exit-2 VerifyError path for a structural problem.
"""
import importlib.util
import os
import struct
import tempfile
import unittest
import zlib

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(_HERE, filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vaa = _load("verify_atlas_alpha", "verify-atlas-alpha.py")

SECTOR = vaa.SECTOR


def _sector_pad(n):
    return (n + SECTOR - 1) // SECTOR * SECTOR


def _align128(n):
    return (n + 127) // 128 * 128


class Tex:
    """A synthetic v10 texture: POT width/height, plus per-level DXT1 block
    bytes and matching RGBA source, both defaulting to a uniform opaque
    4-color (c0 > c1, so it decodes with no transparent selector at all) so
    a test only has to override the specific block/pixel it cares about."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.levels = vaa._chain_levels(width, height)
        self.level_blocks = []  # bytearray per level, blocks concatenated row-major
        self.level_rgba = []    # bytearray per level, pw*ph*4 RGBA

        for level in range(self.levels):
            pw, ph = vaa._level_dims(width, height, level)
            nblocks = (pw // 4) * (ph // 4)
            blocks = bytearray()
            for _ in range(nblocks):
                blocks += struct.pack("<HHI", 0xFFFF, 0x0000, 0)  # 4-color mode: c0 > c1
            self.level_blocks.append(blocks)
            self.level_rgba.append(bytearray([0, 0, 0, 255]) * (pw * ph))  # opaque black

    def set_block(self, level, bx, by, c0, c1, sel):
        pw, _ = vaa._level_dims(self.width, self.height, level)
        blocks_x = pw // 4
        off = (by * blocks_x + bx) * 8
        self.level_blocks[level][off:off + 8] = struct.pack("<HHI", c0, c1, sel)

    def set_pixel_alpha(self, level, x, y, alpha):
        pw, _ = vaa._level_dims(self.width, self.height, level)
        o = (y * pw + x) * 4
        self.level_rgba[level][o + 3] = alpha

    def chain_bytes(self):
        return b"".join(bytes(b) for b in self.level_blocks)

    def digest_for_level(self, level):
        """CRC-32 of this level's CURRENT compressed bytes - the same digest
        buildDxt1MipChain writes into a real dump record (PakManager.cpp),
        recomputed here from whatever set_block() calls have already run so
        a dump built after mutation stays bound to the mutated bytes."""
        return zlib.crc32(bytes(self.level_blocks[level])) & 0xFFFFFFFF

    def dump_records(self, index):
        out = bytearray()
        for level in range(self.levels):
            pw, ph = vaa._level_dims(self.width, self.height, level)
            out += struct.pack("<5I", index, level, pw, ph, self.digest_for_level(level))
            out += bytes(self.level_rgba[level])
        return bytes(out)


def build_archive(textures, version=None, flags=None, corrupt_datalength=None, truncate_by=0):
    if version is None:
        version = vaa.MPAK_LAYOUT_VERSION
    if flags is None:
        flags = vaa.MPAK_FLAG_XBOX | vaa.MPAK_FLAG_NV2A_BLOCK

    dir_bytes = bytearray()
    data_blob = bytearray()
    for i, tex in enumerate(textures):
        chain = tex.chain_bytes()
        data_length = len(chain)
        if corrupt_datalength is not None and i == corrupt_datalength[0]:
            data_length = corrupt_datalength[1]
        dir_bytes += struct.pack("<5I", tex.width, tex.height,
                                  tex.width.bit_length() - 1, tex.height.bit_length() - 1,
                                  data_length)
        data_blob += chain
        data_blob += bytes(_align128(len(chain)) - len(chain))

    header_fields = {name: 0 for name in vaa._HEADER_FIELDS}
    header_fields["version"] = version
    header_fields["headerSize"] = 4 + 4 * len(vaa._HEADER_FIELDS)
    header_fields["bakerBuildId"] = 1
    header_fields["flags"] = flags
    header_fields["textureCount"] = len(textures)
    header_fields["lenTextureDir"] = len(dir_bytes)
    header_fields["lenTextureData"] = len(data_blob)

    sector0 = bytearray(SECTOR)
    struct.pack_into("<i", sector0, 0, vaa.MPAK_BSP_VERSION)  # dheader_t.version
    desc = bytearray(b"MPAK")
    for name in vaa._HEADER_FIELDS:
        desc += struct.pack("<I", header_fields[name])
    sector0[vaa.MPAK_DESCRIPTOR_OFFSET:vaa.MPAK_DESCRIPTOR_OFFSET + len(desc)] = desc

    data = bytearray()
    data += sector0
    data += dir_bytes
    data += bytes(_sector_pad(len(dir_bytes)) - len(dir_bytes))
    data += data_blob
    data += bytes(_sector_pad(len(data_blob)) - len(data_blob))

    if truncate_by:
        data = data[:-truncate_by]

    return bytes(data)


def build_dump(textures, skip_last_record=False):
    out = bytearray()
    out += vaa.DUMP_MAGIC
    out += struct.pack("<I", vaa.DUMP_VERSION)
    for i, tex in enumerate(textures):
        out += tex.dump_records(i)

    if skip_last_record and textures:
        last = textures[-1]
        pw, ph = vaa._level_dims(last.width, last.height, last.levels - 1)
        rec_len = vaa.DUMP_RECORD_HEADER_SIZE + pw * ph * 4
        out = out[:-rec_len]

    return bytes(out)


class VerifyAtlasAlphaTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def _run(self, archive_bytes, dump_bytes):
        mpak_path = os.path.join(self.dir, "test.mpak")
        dump_path = os.path.join(self.dir, "test.rgba")
        with open(mpak_path, "wb") as f:
            f.write(archive_bytes)
        with open(dump_path, "wb") as f:
            f.write(dump_bytes)
        return vaa.main(["verify-atlas-alpha.py", mpak_path, dump_path])

    # (a) clean case -----------------------------------------------------
    def test_a_clean_opaque_texture_passes_with_exact_texel_count(self):
        tex = Tex(8, 4)  # 4 levels: 8x4, 4x2(padded 4x4), 2x1(padded 4x4), 1x1(padded 4x4)
        self.assertEqual(4, tex.levels)
        archive = build_archive([tex])
        dump = build_dump([tex])
        result = vaa.verify(archive, dump)
        self.assertEqual(0, len(result.mismatches))
        self.assertEqual(32 + 16 + 16 + 16, result.texels)
        self.assertEqual(0, self._run(archive, dump))

    # (b) level-0 violation ------------------------------------------------
    def test_b_level0_violation_is_reported_at_the_right_coordinates(self):
        tex = Tex(4, 4)  # 3 levels: 4x4, 2x2(padded), 1x1(padded)
        tex.set_block(level=0, bx=0, by=0, c0=0, c1=1, sel=3)  # texel (0,0) -> selector 3
        tex.set_pixel_alpha(0, 0, 0, 255)
        archive = build_archive([tex])
        dump = build_dump([tex])

        # Independent read of the injected bytes (lessons.md: never trust a
        # new tool's exit code without checking the failure path's own data).
        dir_off = SECTOR
        entry = struct.unpack_from("<5I", archive, dir_off)
        self.assertEqual((4, 4, 2, 2, 24), entry)  # chain: 3 levels * 1 block * 8 bytes = 24
        block0 = archive[SECTOR + _sector_pad(20) : SECTOR + _sector_pad(20) + 8]
        c0, c1, sel = struct.unpack("<HHI", block0)
        self.assertEqual((0, 1, 3), (c0, c1, sel), "the injected 3-color/selector-3 block isn't in the bytes we think")

        result = vaa.verify(archive, dump)
        self.assertEqual(1, len(result.mismatches))
        m = result.mismatches[0]
        self.assertEqual((0, 0, 0), (m.level, m.x, m.y))
        self.assertEqual(1, self._run(archive, dump))

    # (c) deepest-level-only violation --------------------------------------
    def test_c_violation_only_at_the_deepest_level(self):
        tex = Tex(4, 4)  # levels 0,1,2 - level 2 (1x1 padded to 4x4) is deepest
        deepest = tex.levels - 1
        self.assertEqual(2, deepest)
        tex.set_block(level=deepest, bx=0, by=0, c0=0, c1=1, sel=3 << (9 * 2))  # texel idx 9 -> (x=1,y=2)
        tex.set_pixel_alpha(deepest, 1, 2, 255)
        archive = build_archive([tex])
        dump = build_dump([tex])

        result = vaa.verify(archive, dump)
        self.assertEqual(1, len(result.mismatches))
        m = result.mismatches[0]
        self.assertEqual((deepest, 1, 2), (m.level, m.x, m.y))
        self.assertEqual(1, self._run(archive, dump))

    # (d) two textures, violation only in the second, unaligned first chain
    def test_d_second_texture_violation_with_unaligned_first_chain(self):
        tex0 = Tex(4, 4)  # chain length 24 bytes - NOT a multiple of 128
        self.assertNotEqual(0, len(tex0.chain_bytes()) % 128)
        tex1 = Tex(4, 4)
        tex1.set_block(level=0, bx=0, by=0, c0=0, c1=1, sel=3)
        tex1.set_pixel_alpha(0, 0, 0, 255)

        archive = build_archive([tex0, tex1])
        dump = build_dump([tex0, tex1])

        result = vaa.verify(archive, dump)
        self.assertEqual(1, len(result.mismatches))
        self.assertEqual(1, result.mismatches[0].texture)
        self.assertEqual(1, self._run(archive, dump))

    # (e) alpha boundary -----------------------------------------------------
    def test_e_alpha_127_not_a_mismatch_alpha_128_is(self):
        tex = Tex(4, 4)
        # texel idx0 (0,0) alpha 127, texel idx1 (1,0) alpha 128, both selector 3
        sel = 3 | (3 << 2)
        tex.set_block(level=0, bx=0, by=0, c0=0, c1=1, sel=sel)
        tex.set_pixel_alpha(0, 0, 0, 127)
        tex.set_pixel_alpha(0, 1, 0, 128)
        archive = build_archive([tex])
        dump = build_dump([tex])

        result = vaa.verify(archive, dump)
        self.assertEqual(1, len(result.mismatches))
        m = result.mismatches[0]
        self.assertEqual((0, 1, 0), (m.level, m.x, m.y))
        self.assertEqual(1, self._run(archive, dump))

    # (f) legitimate cutout ----------------------------------------------------
    def test_f_alpha_0_decoded_transparent_is_not_a_mismatch(self):
        tex = Tex(4, 4)
        tex.set_block(level=0, bx=0, by=0, c0=0, c1=1, sel=3)
        tex.set_pixel_alpha(0, 0, 0, 0)
        archive = build_archive([tex])
        dump = build_dump([tex])

        result = vaa.verify(archive, dump)
        self.assertEqual(0, len(result.mismatches))
        self.assertEqual(0, self._run(archive, dump))

    # (g) exit-2 cases, one apiece --------------------------------------------
    def test_g_wrong_version_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex], version=9)
        dump = build_dump([tex])
        self.assertEqual(2, self._run(archive, dump))
        with self.assertRaises(vaa.VerifyError):
            vaa.verify(archive, dump)

    def test_g_flags_missing_xbox_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex], flags=vaa.MPAK_FLAG_NV2A_BLOCK)
        dump = build_dump([tex])
        self.assertEqual(2, self._run(archive, dump))

    def test_g_file_truncated_by_a_sector_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex], truncate_by=SECTOR)
        dump = build_dump([tex])
        self.assertEqual(2, self._run(archive, dump))

    def test_g_datalength_disagreeing_with_the_computed_chain_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex], corrupt_datalength=(0, 999))
        dump = build_dump([tex])
        self.assertEqual(2, self._run(archive, dump))

    def test_g_dump_missing_its_last_record_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex])
        dump = build_dump([tex], skip_last_record=True)
        self.assertEqual(2, self._run(archive, dump))

    def test_g_dump_record_with_wrong_dims_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex])
        dump = bytearray(build_dump([tex]))
        # record 0 starts right after the 8-byte dump header; width is at +8.
        width_off = 8 + 8
        struct.pack_into("<I", dump, width_off, 999)
        self.assertEqual(2, self._run(archive, bytes(dump)))

    def test_g_texture_count_zero_is_exit_2(self):
        archive = build_archive([])
        dump = build_dump([])
        self.assertEqual(2, self._run(archive, dump))

    # QA round 2, codex-review finding 1: _parse_dump unpacked the 4-byte
    # version field right after checking only the 4-byte magic, so a dump
    # truncated to 4-7 bytes raised an uncaught struct.error - Python's
    # default exit code 1, which this tool reserves for "a real mismatch was
    # found". Both of these must resolve to exit 2 (a structural problem)
    # instead, and via the documented VerifyError path, never an unhandled
    # exception.
    def test_g_dump_header_is_magic_only_four_bytes_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex])
        dump = vaa.DUMP_MAGIC  # exactly 4 bytes: magic, no version field at all
        self.assertEqual(2, self._run(archive, dump))
        with self.assertRaises(vaa.VerifyError):
            vaa.verify(archive, dump)

    def test_g_dump_header_version_field_partial_is_exit_2(self):
        tex = Tex(4, 4)
        archive = build_archive([tex])
        dump = vaa.DUMP_MAGIC + b"\x02\x00"  # magic + 2 of the version field's 4 bytes
        self.assertEqual(2, self._run(archive, dump))
        with self.assertRaises(vaa.VerifyError):
            vaa.verify(archive, dump)

    # (h) dump/archive binding - codex-review finding, QA round 1: a dump
    # built from a DIFFERENT compressed chain (same texture dimensions) must
    # not be silently accepted against today's archive just because record
    # order and dimensions line up.
    def test_h_dump_digest_from_a_different_archive_is_exit_2(self):
        tex_a = Tex(4, 4)  # archive under test: default uniform opaque 4-color blocks
        archive_a = build_archive([tex_a])

        tex_b = Tex(4, 4)  # same dimensions/level count, but different compressed bytes
        tex_b.set_block(level=0, bx=0, by=0, c0=0x1234, c1=0x5678, sel=0xAAAAAAAA)
        dump_b = build_dump([tex_b])

        # Confirm by an independent read that the two chains really do differ
        # (lessons.md: never trust a new check's exit code without checking
        # the underlying bytes too) - otherwise this test would prove nothing.
        self.assertNotEqual(tex_a.chain_bytes(), tex_b.chain_bytes())
        self.assertNotEqual(tex_a.digest_for_level(0), tex_b.digest_for_level(0))

        self.assertEqual(2, self._run(archive_a, dump_b))
        with self.assertRaises(vaa.VerifyError):
            vaa.verify(archive_a, dump_b)

    # (i) mismatch-count memory bound - codex-review finding, QA round 1: only
    # the first MAX_RETAINED_MISMATCHES Mismatch objects may be retained, but
    # the reported count and exit code must still reflect every mismatch.
    def test_i_mismatch_count_beyond_retained_cap_stays_correct(self):
        tex = Tex(64, 32)  # level 0: 16*8 = 128 blocks = 2048 texels, all made to mismatch
        blocks_x, blocks_y = 64 // 4, 32 // 4
        for by in range(blocks_y):
            for bx in range(blocks_x):
                tex.set_block(level=0, bx=bx, by=by, c0=0, c1=1, sel=0xFFFFFFFF)  # every texel -> selector 3
        for y in range(32):
            for x in range(64):
                tex.set_pixel_alpha(0, x, y, 255)  # every source texel opaque

        archive = build_archive([tex])
        dump = build_dump([tex])

        result = vaa.verify(archive, dump)
        self.assertEqual(64 * 32, result.mismatch_count)
        self.assertGreater(result.mismatch_count, vaa.MAX_RETAINED_MISMATCHES)
        self.assertEqual(vaa.MAX_RETAINED_MISMATCHES, len(result.mismatches),
                          "only the first MAX_RETAINED_MISMATCHES Mismatch objects may be retained")
        self.assertEqual(1, self._run(archive, dump))

    def test_usage_error_is_exit_2(self):
        self.assertEqual(2, vaa.main(["verify-atlas-alpha.py", "only-one-arg"]))


if __name__ == "__main__":
    unittest.main()
