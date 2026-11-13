"""Tests for tools/save_layout_check.py (docs/r3d/fork-plan.md, XS.0).

The XBEs are built here, small, with what the check reads: the server's export table, the SDK's dispatch table and
some function bodies. A real-XBE test runs when R3D_PUBLIC_XBES names a folder holding xbe-*/default.xbe.
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
FUNC_OFFSETS = [0x1000 + 0x40 * i for i in range(6)]  # where the test functions sit, before any shift
ANCHOR_OFFSET = 0x100  # the lowest export


def default_funcs(shift):
    """six functions with distinct bodies; the first has a call operand that moves with the image, as a real call
    to something outside the block does"""
    out = {}
    for i, off in enumerate(FUNC_OFFSETS):
        out[off] = bytes((0x40 + 7 * i + k) & 0xFF for k in range(slc.WINDOW))
    first = bytearray(out[FUNC_OFFSETS[0]])
    first[1:5] = struct.pack("<I", 0x1000 + shift)
    out[FUNC_OFFSETS[0]] = bytes(first)
    return out


def build_xbe(exports=None, dispatch=None, shift=0, strip_dispatch=False, funcs=None):
    """exports: {name: offset in .text}; dispatch: [offset in .text] (the first is GameDLLInit); funcs: {offset: bytes}.
    shift moves all of it by the same amount, the way a rigid move of the server block does."""
    if exports is None:
        exports = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES)}
    if dispatch is None:
        dispatch = [0x2000 + 0x30 * i for i in range(DISPATCH)]
    if funcs is None:
        funcs = default_funcs(shift)
    text = bytearray(b"\xcc" * 0x4000)
    for off, body in funcs.items():
        text[off + shift:off + shift + len(body)] = body
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


def baseline_text(shifts=(0, 0x200, 0x630), names=None):
    """a baseline made from the first build, with function masks from the differences between all of them"""
    builds = []
    for sh in shifts:
        data, _ = build_xbe(shift=sh)
        builds.append((slc.Xbe(data), TEXT_VA + ANCHOR_OFFSET + sh))
    first, gi = build_xbe(shift=shifts[0])
    names = names or ["fn_%d" % i for i in range(len(FUNC_OFFSETS))]
    functions = slc.derive_functions(builds, [(o - ANCHOR_OFFSET, n) for o, n in zip(FUNC_OFFSETS, names)])
    return slc.make_baseline(slc.Xbe(first), "xbe-test", "0" * 64, gi, functions)


class Layout(unittest.TestCase):
    def setUp(self):
        self.base = slc.parse_baseline(baseline_text())

    def problems(self, data):
        return slc.check(slc.Xbe(data), self.base)

    def test_the_baseline_passes_against_its_own_xbe(self):
        data, _ = build_xbe()
        self.assertEqual(self.problems(data), [])
        self.assertEqual(len(self.base["exports"]), len(NAMES))
        self.assertEqual(len(self.base["dispatch"]), DISPATCH)
        self.assertEqual(self.base["dispatch"][0], self.base["gamedllinit"])
        self.assertEqual(len(self.base["functions"]), len(FUNC_OFFSETS))

    def test_the_whole_block_moving_together_passes(self):
        # what the three public builds did: every server function moved by one constant, and a call operand with it
        moved, _ = build_xbe(shift=0x400)
        self.assertEqual(self.problems(moved), [])

    def test_one_export_moving_fails_and_names_it(self):
        ex = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_20"] += 16
        data, _ = build_xbe(exports=ex)
        p = self.problems(data)
        self.assertEqual(len(p), 1)
        self.assertIn("monster_20 +16", p[0])

    def test_a_function_growing_moves_everything_after_it(self):
        ex = {n: ANCHOR_OFFSET + 0x40 * i + (0x20 if i > 10 else 0) for i, n in enumerate(NAMES)}
        data, _ = build_xbe(exports=ex)
        p = self.problems(data)
        self.assertTrue(p and "moved relative to" in p[0], p)

    def test_an_export_going_missing_fails(self):
        ex = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES) if n != "monster_30"}
        data, _ = build_xbe(exports=ex)
        self.assertTrue(any("are gone" in x and "monster_30" in x for x in self.problems(data)))

    def test_an_export_appended_at_the_end_is_fine(self):
        ex = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_new"] = ANCHOR_OFFSET + 0x40 * len(NAMES)
        data, _ = build_xbe(exports=ex)
        self.assertEqual(self.problems(data), [])

    def test_the_anchor_going_missing_fails(self):
        # an anchor that is not the marker entity, as in the real baseline (monster_flyer)
        ex = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_first"] = 0x80
        data, gi = build_xbe(exports=ex)
        base = slc.parse_baseline(slc.make_baseline(slc.Xbe(data), "x", "0" * 64, gi))
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
        data, _ = build_xbe(exports={"monster_x%d" % i: ANCHOR_OFFSET + 0x40 * i for i in range(45)})
        p = self.problems(data)
        self.assertEqual(len(p), 1)
        self.assertIn("no server export table", p[0])

    def test_something_that_is_not_an_xbe(self):
        data, _ = build_xbe()
        with self.assertRaises(slc.XbeError):
            slc.Xbe(b"MZ" + b"\0" * 1000)
        with self.assertRaises(slc.XbeError):
            slc.Xbe(data[:0x300])
        broken = bytearray(data)
        struct.pack_into("<I", broken, 0x200 + 12, 0x7FFFFFF0)  # a section raw address far past the file
        with self.assertRaises(slc.XbeError):
            slc.Xbe(bytes(broken))


class FunctionBodies(unittest.TestCase):
    """the part that sees a callback which is neither exported nor in the dispatch table"""

    def setUp(self):
        self.base = slc.parse_baseline(baseline_text())

    def run_with(self, funcs):
        data, _ = build_xbe(funcs=funcs)
        return slc.check(slc.Xbe(data), self.base)

    def test_a_changed_function_fails_and_names_it(self):
        funcs = default_funcs(0)
        body = bytearray(funcs[FUNC_OFFSETS[3]])
        body[10] ^= 0xFF
        funcs[FUNC_OFFSETS[3]] = bytes(body)
        p = self.run_with(funcs)
        self.assertEqual(len(p), 1)
        self.assertIn("1 of 6 server function(s)", p[0])
        self.assertIn("fn_3", p[0])

    def test_two_functions_trading_places_fails(self):
        funcs = default_funcs(0)
        a, b = FUNC_OFFSETS[1], FUNC_OFFSETS[4]
        funcs[a], funcs[b] = funcs[b], funcs[a]
        p = self.run_with(funcs)
        self.assertEqual(len(p), 1)
        self.assertIn("2 of 6", p[0])

    def test_a_function_moved_inside_the_block_fails(self):
        funcs = default_funcs(0)
        funcs[FUNC_OFFSETS[2] + 0x10] = funcs.pop(FUNC_OFFSETS[2])  # 16 bytes later; the old place is padding now
        self.assertTrue(any("do not start with" in x for x in self.run_with(funcs)))

    def test_a_masked_byte_may_differ(self):
        funcs = default_funcs(0)
        body = bytearray(funcs[FUNC_OFFSETS[0]])
        body[2] ^= 0x55  # inside the call operand the three builds disagreed on
        funcs[FUNC_OFFSETS[0]] = bytes(body)
        self.assertEqual(self.run_with(funcs), [])

    def test_a_function_gone_from_the_image_fails(self):
        funcs = default_funcs(0)
        del funcs[FUNC_OFFSETS[5]]
        self.assertTrue(any("do not start with" in x for x in self.run_with(funcs)))

    def test_masks_come_from_two_or_more_builds(self):
        data, _ = build_xbe()
        with self.assertRaises(slc.XbeError):
            slc.derive_functions([(slc.Xbe(data), TEXT_VA + ANCHOR_OFFSET)], [(0x10, "f")])

    def test_a_function_with_too_little_left_unmasked_is_not_recorded(self):
        # every byte of this body depends on the shift, so nothing stable remains
        def funcs_for(shift):
            f = default_funcs(shift)
            f[FUNC_OFFSETS[1]] = bytes((shift + k) & 0xFF for k in range(slc.WINDOW))
            return f
        builds = []
        for sh in (0, 0x200, 0x630):
            data, _ = build_xbe(shift=sh, funcs=funcs_for(sh))
            builds.append((slc.Xbe(data), TEXT_VA + ANCHOR_OFFSET + sh))
        got = slc.derive_functions(builds, [(o - ANCHOR_OFFSET, "f%d" % i) for i, o in enumerate(FUNC_OFFSETS)])
        self.assertEqual([n for _, n, _ in got], ["f0", "f2", "f3", "f4", "f5"])

    def test_the_pattern_keeps_the_stable_bytes_and_masks_the_rest(self):
        pat = [p for o, p, n in self.base["functions"] if n == "fn_0"][0]
        self.assertEqual(len(pat), 2 * slc.WINDOW)
        # the call operand is 0x1000, 0x1200 and 0x1630 in the three builds: its low two bytes differ, its high two
        # (zero) do not, and masking is per byte
        self.assertEqual(pat[2:6], "????")
        self.assertEqual(pat[6:10], "0000")
        self.assertNotIn("?", pat[10:])

    def test_a_malformed_pattern_is_refused(self):
        text = baseline_text()
        good = [l for l in text.splitlines() if l.startswith("fn ")][0]
        for bad in (good.replace("fn ", "fn ", 1)[:20], good.replace(good.split()[2], "zz" + good.split()[2][2:])):
            with self.assertRaises(slc.XbeError, msg=bad):
                slc.parse_baseline(text.replace(good, bad))


class BaselineFile(unittest.TestCase):
    def test_the_committed_baseline(self):
        with open(slc.BASELINE, encoding="utf-8") as f:
            b = slc.parse_baseline(f.read())
        self.assertEqual(len(b["exports"]), 251)
        self.assertEqual(b["dispatch"][0], b["gamedllinit"])
        self.assertEqual(b["exports"][b["anchor"]], 0)
        self.assertIn("worldspawn", b["exports"])
        offs = list(b["exports"].values())
        self.assertEqual(offs, sorted(offs))  # written in address order
        self.assertGreater(len(b["functions"]), 2900)  # the server module's functions, patterns from three builds
        fn_offsets = {o for o, p, n in b["functions"]}
        self.assertIn(b["gamedllinit"], fn_offsets)  # GameDLLInit itself is checked
        self.assertTrue(all(sum(1 for i in range(0, len(p), 2) if p[i:i + 2] != "??") >= slc.MIN_STABLE
                            for o, p, n in b["functions"]))
        # nothing is left out silently: every server symbol is either checked or listed as skipped, and they add up
        self.assertEqual(b["symbols"], len(b["functions"]) + len(b["skipped"]))
        self.assertEqual(b["symbols"], 2998)
        self.assertLess(len(b["skipped"]), 20)
        self.assertFalse({o for o, _ in b["skipped"]} & fn_offsets)

    def test_no_skipped_function_is_ever_a_saved_callback(self):
        """the functions the baseline cannot fingerprint must not be think/touch/use/blocked/move-done targets"""
        import re
        repo = os.path.dirname(HERE)
        dlls = next((d for d in (os.path.join(repo, "deps", "hlsdk", "dlls"),) if os.path.isdir(d)
                     and os.listdir(d)), None)
        if dlls is None:
            self.skipTest("needs the deps/hlsdk submodule")
        pat = re.compile(r"Set(?:Think|Touch|Use|Blocked|MoveDone)\s*\(\s*&?\s*(?:\w+\s*::\s*)?(\w+)")
        assigned = set()
        for dp, _, files in os.walk(dlls):
            for fn in files:
                if fn.endswith((".cpp", ".h")):
                    with open(os.path.join(dp, fn), encoding="utf-8", errors="replace") as f:
                        assigned.update(m.group(1) for m in pat.finditer(f.read()))
        self.assertGreater(len(assigned), 150)  # the scan itself works
        with open(slc.BASELINE, encoding="utf-8") as f:
            b = slc.parse_baseline(f.read())
        for _, name in b["skipped"]:
            m = re.match(r"sv_\?(\w+)@", name)
            bare = m.group(1) if m else name
            self.assertNotIn(bare, assigned, name)

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
                f.write(baseline_text())
            slc.require(data, path, None)  # passes
            ex = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES)}
            ex["monster_05"] += 16
            moved, _ = build_xbe(exports=ex)
            with self.assertRaises(SystemExit) as cm:
                slc.require(moved, path, None)
            self.assertIn("monster_05 +16", str(cm.exception))
            self.assertIn("no build may ever make an existing save unloadable", str(cm.exception))
            with self.assertRaises(SystemExit):
                slc.require(b"not an xbe", path, None)
            with self.assertRaises(SystemExit):
                slc.require(data, os.path.join(tmp, "missing.txt"), None)  # no baseline is a failure, not a pass

    def test_gameinit_from_a_link_map(self):
        m = (" 0001:0003eef0       sv_?GameDLLInit@@YAXXZ     000000000002fef0     deps_hlsdk_dlls_game.obj\n"
             " 0001:0003eef8       ?Other@@YAXXZ              000000000002fef8     x.obj\n"
             " 0002:00000010       sv_?GameDLLInit@@YAXXZ     0000000000999999     not_code.obj\n")
        self.assertEqual(slc.gameinit_from_map(m), 0x2fef0)
        with self.assertRaises(slc.XbeError):
            slc.gameinit_from_map(m + " 0001:00000001 sv_?GameDLLInit2@@YAXXZ 0000000000000001 y\n")

    def test_server_symbols_from_a_link_map(self):
        m = (" 0001:0003eef0       sv_?A@@YAXXZ     000000000002fef0     a.obj\n"
             " 0001:0003eef0       sv_?B@@YAXXZ     000000000002fef0     b.obj\n"
             " 0001:0003eef8       cl_?C@@YAXXZ     000000000002fef8     c.obj\n"
             " 0002:00000010       sv_?D@@YAXXZ     0000000000999999     data.obj\n")
        self.assertEqual(slc.server_symbols_from_map(m), {0x2fef0: "sv_?A@@YAXXZ"})  # one name per address, code only


class Provenance(unittest.TestCase):
    """the baseline is the policy, so what makes it the policy is checked: its digest, and the builds it came from"""

    def committed(self):
        with open(slc.BASELINE, encoding="utf-8") as f:
            return f.read()

    def test_the_committed_baseline_is_the_pinned_one(self):
        text = self.committed()
        self.assertEqual(slc.provenance_problems(slc.parse_baseline(text), text, slc.BASELINE_SHA256), [])
        self.assertEqual(slc.text_digest(text), slc.BASELINE_SHA256)

    def test_the_digest_ignores_line_endings(self):
        text = self.committed()
        self.assertEqual(slc.text_digest(text.replace("\n", "\r\n")), slc.text_digest(text))

    def test_an_edited_baseline_is_refused(self):
        text = self.committed()
        base = slc.parse_baseline(text)
        first_fn = [l for l in text.splitlines() if l.startswith("fn ")][0]
        nl = chr(10)
        for edited in (text + "# a comment" + nl,
                       text.replace("export monster_flyer_flock 16", "export monster_flyer_flock 32"),
                       text.replace(first_fn + nl, "")):
            self.assertNotEqual(edited, text)
            p = slc.provenance_problems(base, edited, slc.BASELINE_SHA256)
            self.assertTrue(any("pinned digest" in x for x in p), p)

    def test_a_baseline_regenerated_to_match_a_candidate_is_refused(self):
        # the candidate moves an export; a baseline made from the candidate would pass it on its own terms
        ex = {n: ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(NAMES)}
        ex["monster_07"] += 16
        moved, gi = build_xbe(exports=ex)
        own = slc.make_baseline(slc.Xbe(moved), "xbe-cdd64653", slc.PUBLIC_BUILDS["xbe-cdd64653"], gi)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "own.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write(own)
            slc.require(moved, path, None)  # on its own terms it passes: that is the hole being closed
            with self.assertRaises(SystemExit) as cm:
                slc.require(moved, path)  # the default policy pins the digest
            self.assertIn("pinned digest", str(cm.exception))

    def test_a_baseline_from_some_other_xbe_is_refused(self):
        text = self.committed().replace(slc.PUBLIC_BUILDS["xbe-cdd64653"], "0" * 64, 1)
        p = slc.provenance_problems(slc.parse_baseline(text), text, None)
        self.assertTrue(any("not a pinned public build" in x for x in p), p)

    def test_masks_must_come_from_the_other_public_builds(self):
        text = self.committed()
        first = [l for l in text.splitlines() if l.startswith("masks ")][0]
        p = slc.provenance_problems(slc.parse_baseline(text.replace(first, "masks xbe-1f4eedc1 " + "0" * 64)), text, None)
        self.assertTrue(any("masks were not made" in x for x in p), p)

    def test_the_baseline_command_takes_only_the_public_build(self):
        import subprocess
        data, gi = build_xbe()
        with tempfile.TemporaryDirectory() as tmp:
            xbe = os.path.join(tmp, "a.xbe")
            with open(xbe, "wb") as f:
                f.write(data)
            mp = os.path.join(tmp, "a.map")
            with open(mp, "w") as f:
                f.write(" 0001:00000000 sv_?GameDLLInit@@YAXXZ %016x x.obj\n" % gi)
            r = subprocess.run([sys.executable, os.path.join(HERE, "save_layout_check.py"), "baseline", xbe, "--map", mp,
                                "--label", "xbe-cdd64653", "--also-xbe", xbe, "--out", os.path.join(tmp, "o.txt")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("is made from the public xbe-cdd64653 XBE", r.stdout)
            self.assertFalse(os.path.exists(os.path.join(tmp, "o.txt")))

    def test_the_symbol_count_must_add_up(self):
        text = self.committed().replace("symbols 2998", "symbols 2999")
        with self.assertRaises(slc.XbeError):
            slc.parse_baseline(text)


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
