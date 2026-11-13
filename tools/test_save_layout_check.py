"""Tests for tools/save_layout_check.py (docs/r3d/fork-plan.md, XS.0).

The XBEs are built here, small, with the two tables the check reads: the server's export table and the SDK's
dispatch table. A real-XBE test runs when R3D_PUBLIC_XBES names a folder holding xbe-*/default.xbe.
"""

import glob
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_layout_check as slc  # noqa: E402

BASE = 0x10000
TEXT_VA, RDATA_VA, DATA_VA = 0x11000, 0x15000, 0x16000
NAMES = ["worldspawn", "player"] + ["monster_%02d" % i for i in range(48)]
DISPATCH = 30


def build_xbe(exports=None, dispatch=None, shift=0, strip_dispatch=False):
    """exports: {name: offset in .text}; dispatch: [offset in .text] (the first is GameDLLInit); shift moves all of
    both by the same amount, the way a rigid move of the server block does."""
    if exports is None:
        exports = {n: 0x100 + 0x40 * i for i, n in enumerate(NAMES)}
    if dispatch is None:
        dispatch = [0x2000 + 0x30 * i for i in range(DISPATCH)]
    text = bytearray(b"\xcc" * 0x4000)
    rdata = bytearray()
    name_va = {}
    for n in exports:
        name_va[n] = RDATA_VA + len(rdata)
        rdata += n.encode() + b"\0"
    rdata += b"\0" * (-len(rdata) % 16)
    data = bytearray(struct.pack("<4I", 0, 0, 0, 0))
    for n, off in exports.items():
        data += struct.pack("<II", name_va[n], TEXT_VA + off + shift)
    data += struct.pack("<4I", 0, 0, 0, 0)
    dispatch_va = DATA_VA + len(data)
    if not strip_dispatch:
        data += b"".join(struct.pack("<I", TEXT_VA + o + shift) for o in dispatch)
    data += struct.pack("<4I", 0, 0, 0, 0)

    secs = [(".text", 4, TEXT_VA, text), (".rdata", 0, RDATA_VA, rdata), (".data", 2, DATA_VA, data)]
    head = bytearray(0x1000)
    head[0:4] = b"XBEH"
    struct.pack_into("<III", head, 0x104, BASE, 0x1000, 0x8000)
    struct.pack_into("<II", head, 0x11C, len(secs), BASE + 0x200)
    raw = 0x1000
    body = b""
    for i, (name, flags, va, blob) in enumerate(secs):
        nm = 0x400 + 16 * i
        head[nm:nm + len(name) + 1] = name.encode() + b"\0"
        struct.pack_into("<IIIIII", head, 0x200 + 56 * i, flags, va, len(blob), raw + len(body), len(blob), BASE + nm)
        body += bytes(blob)
    return bytes(head) + body, TEXT_VA + dispatch[0] + shift


def baseline_for(data, gameinit_va):
    return slc.parse_baseline(slc.make_baseline(slc.Xbe(data), "xbe-test", "0" * 64, gameinit_va))


class Layout(unittest.TestCase):
    def setUp(self):
        self.data, gi = build_xbe()
        self.base = baseline_for(self.data, gi)

    def problems(self, data):
        return slc.check(slc.Xbe(data), self.base)

    def test_the_baseline_passes_against_its_own_xbe(self):
        self.assertEqual(self.problems(self.data), [])
        self.assertEqual(len(self.base["exports"]), len(NAMES))
        self.assertEqual(len(self.base["dispatch"]), DISPATCH)
        self.assertEqual(self.base["dispatch"][0], self.base["gamedllinit"])

    def test_the_whole_block_moving_together_passes(self):
        # what the three public builds did: every server function moved by one constant
        moved, _ = build_xbe(shift=0x630)
        self.assertEqual(self.problems(moved), [])

    def test_one_export_moving_fails_and_names_it(self):
        ex = {n: 0x100 + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_20"] += 16
        data, _ = build_xbe(exports=ex)
        p = self.problems(data)
        self.assertEqual(len(p), 1)
        self.assertIn("monster_20 +16", p[0])

    def test_a_function_growing_moves_everything_after_it(self):
        ex = {n: 0x100 + 0x40 * i + (0x20 if i > 10 else 0) for i, n in enumerate(NAMES)}
        data, _ = build_xbe(exports=ex)
        p = self.problems(data)
        self.assertTrue(p and "moved relative to" in p[0], p)

    def test_an_export_going_missing_fails(self):
        ex = {n: 0x100 + 0x40 * i for i, n in enumerate(NAMES) if n != "monster_30"}
        data, _ = build_xbe(exports=ex)
        self.assertTrue(any("are gone" in x and "monster_30" in x for x in self.problems(data)))

    def test_an_export_appended_at_the_end_is_fine(self):
        ex = {n: 0x100 + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_new"] = 0x100 + 0x40 * len(NAMES)
        data, _ = build_xbe(exports=ex)
        self.assertEqual(self.problems(data), [])

    def test_the_anchor_going_missing_fails(self):
        # an anchor that is not the marker entity, as in the real baseline (monster_flyer)
        ex = {n: 0x100 + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_first"] = 0x80
        data, gi = build_xbe(exports=ex)
        base = baseline_for(data, gi)
        self.assertEqual(base["anchor"], "monster_first")
        del ex["monster_first"]
        gone, _ = build_xbe(exports=ex)
        p = slc.check(slc.Xbe(gone), base)
        self.assertEqual(len(p), 1)
        self.assertIn("anchor export 'monster_first' is gone", p[0])

    def test_gamedllinit_moving_fails(self):
        d = [0x2000 + 0x30 * i for i in range(DISPATCH)]
        d[0] += 16
        data, _ = build_xbe(dispatch=d)
        self.assertTrue(any("dispatch table" in x for x in self.problems(data)))

    def test_a_later_dispatch_entry_moving_fails(self):
        d = [0x2000 + 0x30 * i for i in range(DISPATCH)]
        d[17] += 4
        data, _ = build_xbe(dispatch=d)
        self.assertTrue(any("dispatch table" in x for x in self.problems(data)))

    def test_a_missing_dispatch_table_fails(self):
        data, _ = build_xbe(strip_dispatch=True)
        self.assertTrue(any("dispatch table" in x for x in self.problems(data)))

    def test_no_export_table_fails(self):
        data, _ = build_xbe(exports={"monster_x%d" % i: 0x100 + 0x40 * i for i in range(45)})
        p = self.problems(data)
        self.assertEqual(len(p), 1)
        self.assertIn("no server export table", p[0])

    def test_something_that_is_not_an_xbe(self):
        with self.assertRaises(slc.XbeError):
            slc.Xbe(b"MZ" + b"\0" * 1000)
        with self.assertRaises(slc.XbeError):
            slc.Xbe(self.data[:0x300])
        broken = bytearray(self.data)
        struct.pack_into("<I", broken, 0x200 + 12, 0x7FFFFFF0)  # a section raw address far past the file
        with self.assertRaises(slc.XbeError):
            slc.Xbe(bytes(broken))


class BaselineFile(unittest.TestCase):
    def test_the_committed_baseline(self):
        with open(slc.BASELINE, encoding="utf-8") as f:
            b = slc.parse_baseline(f.read())
        self.assertEqual(len(b["exports"]), 251)
        self.assertEqual(b["dispatch"][0], b["gamedllinit"])
        self.assertEqual(b["exports"][b["anchor"]], 0)
        self.assertIn("worldspawn", b["exports"])
        # monotone: it is written in address order
        offs = list(b["exports"].values())
        self.assertEqual(offs, sorted(offs))

    def test_a_bad_baseline_is_refused(self):
        good = "build x 0\nanchor a\ngamedllinit 4\ndispatch " + " ".join(str(4 + i) for i in range(25)) + \
            "\nexport a 0\nexport b 16\n"
        slc.parse_baseline(good)
        for bad in (good.replace("export a 0", "export a 8"),  # the anchor is not at 0
                    good.replace("gamedllinit 4", "gamedllinit 8"),  # dispatch does not start at GameDLLInit
                    good.replace("anchor a", ""),
                    good + "frobnicate 1\n",
                    good.replace("export b 16", "export b sixteen")):
            with self.assertRaises(slc.XbeError, msg=bad):
                slc.parse_baseline(bad)

    def test_the_release_path_refuses_and_says_why(self):
        data, gi = build_xbe()
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "b.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write(slc.make_baseline(slc.Xbe(data), "xbe-test", "0" * 64, gi))
            slc.require(data, path)  # passes
            ex = {n: 0x100 + 0x40 * i for i, n in enumerate(NAMES)}
            ex["monster_05"] += 16
            moved, _ = build_xbe(exports=ex)
            with self.assertRaises(SystemExit) as cm:
                slc.require(moved, path)
            self.assertIn("monster_05 +16", str(cm.exception))
            self.assertIn("no build may ever make an existing save unloadable", str(cm.exception))
            with self.assertRaises(SystemExit):
                slc.require(b"not an xbe", path)
            with self.assertRaises(SystemExit):
                slc.require(data, os.path.join(tmp, "missing.txt"))  # no baseline is a failure, not a pass

    def test_gameinit_from_a_link_map(self):
        m = (" 0001:0003eef0       sv_?GameDLLInit@@YAXXZ     000000000002fef0     deps_hlsdk_dlls_game.obj\n"
             " 0001:0003eef8       ?Other@@YAXXZ              000000000002fef8     x.obj\n"
             " 0002:00000010       sv_?GameDLLInit@@YAXXZ     0000000000999999     not_code.obj\n")
        self.assertEqual(slc.gameinit_from_map(m), 0x2fef0)
        with self.assertRaises(slc.XbeError):
            slc.gameinit_from_map(m.replace("2fef0", "2fef0", 1) + " 0001:00000001 sv_?GameDLLInit2@@YAXXZ 0000000000000001 y\n")


class PublicBuilds(unittest.TestCase):
    """The three published XBEs, when their folder is given: they must all pass the committed baseline."""

    def test_the_public_xbes_keep_the_layout(self):
        root = os.environ.get("R3D_PUBLIC_XBES")
        if not root:
            self.skipTest("R3D_PUBLIC_XBES is not set")
        found = sorted(glob.glob(os.path.join(root, "*", "default.xbe")))
        self.assertTrue(found, "no */default.xbe under %s" % root)
        for path in found:
            problems, _ = slc.check_file(path)
            self.assertEqual(problems, [], path)


if __name__ == "__main__":
    unittest.main()
