"""XS.4 live level: the parts of tools/save_live_gate.py that need no xemu. Run: python -m unittest discover -s tools -p "test_save_live_gate.py" """
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_live_gate as live  # noqa: E402


def make_save(mapname):
    toks = [b"GameHeader", b"mapName", b"comment", b"mapCount"]
    table = b"".join(t + b"\0" for t in toks)
    name = mapname.encode().ljust(32, b"\0")
    comment = b"#C1A4TITLE".ljust(80, b"\0")
    fields = (struct.pack("<hh", 32, 1) + name + struct.pack("<hh", 80, 2) + comment + struct.pack("<hh", 4, 3) + b"\1\0\0\0")
    body = table + struct.pack("<hh", 4, 0) + bytes([3, 0, 0, 0]) + fields
    return struct.pack("<5i", 0x56415347, 113, len(body) - len(table), len(toks), len(table)) + body


class Header(unittest.TestCase):
    def test_reads_the_map_the_engine_reads(self):
        self.assertEqual(live.header_map(make_save("c1a4e")), "c1a4e")

    def test_garbage_gives_none(self):
        self.assertIsNone(live.header_map(b"nope"))
        self.assertIsNone(live.header_map(b""))

    def test_the_real_fixtures_have_a_map(self):
        folder = os.environ.get("R3D_SAVE_FIXTURES")
        if not folder or not os.path.isdir(folder):
            self.skipTest("R3D_SAVE_FIXTURES not set")
        n = 0
        for f in os.listdir(folder):
            if f.endswith(".sav"):
                with open(os.path.join(folder, f), "rb") as fh:
                    self.assertTrue(live.header_map(fh.read()), f)
                n += 1
        self.assertGreater(n, 0)


class Played(unittest.TestCase):
    TEXT = "\n".join(["map  played avg", "(before first map)      - (no frames in play)", "c1a4e   6.9   59.7",
                      "avg fps in play 59.7", "", "c1a4e  48000  other table", ""])

    def test_seconds_for_the_map_and_zero_for_others(self):
        self.assertEqual(live.played(self.TEXT, "c1a4e"), 6.9)
        self.assertEqual(live.played(self.TEXT, "c2a1"), 0.0)

    def test_a_map_with_a_row_of_no_frames_and_a_row_of_play_counts_the_play(self):
        text = "\n".join(["map played avg", "c2a2e                 - (no frames in play)",
                          "c2a2e                 3.3   60.0   60.0", ""])
        self.assertEqual(live.played(text, "c2a2e"), 3.3)


class Evidence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.xbe = b"XBE-bytes"
        self.expected = os.path.join(self.tmp.name, "expected.txt")
        self.h1, self.h2 = "a" * 64, "b" * 64
        with open(self.expected, "w", encoding="utf-8") as f:
            f.write("fixture %s one.sav X=1\nfixture %s c1a4g.HL1 X=1\n" % (self.h1, self.h2))
        self.ev = os.path.join(self.tmp.name, "ev.txt")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, xbe=None, rows=None):
        with open(self.ev, "w", encoding="utf-8") as f:
            f.write("xbe %s\n" % live.sha(xbe or self.xbe))
            for r in rows if rows is not None else ["fixture %s one.sav c1a4e 6.9 PASS" % self.h1]:
                f.write(r + "\n")

    def test_passes_for_this_xbe_with_every_sav_loaded(self):
        self.write()
        self.assertEqual(live.evidence_problems(self.xbe, self.ev, self.expected), [])

    def test_another_xbe_is_refused(self):
        self.write(xbe=b"other")
        self.assertIn("another XBE", live.evidence_problems(self.xbe, self.ev, self.expected)[0])

    def test_a_missing_or_failed_fixture_is_refused(self):
        self.write(rows=[])
        self.assertIn("was not loaded", live.evidence_problems(self.xbe, self.ev, self.expected)[0])
        self.write(rows=["fixture %s one.sav c1a4e 0.0 FAIL" % self.h1])
        self.assertIn("did not load", live.evidence_problems(self.xbe, self.ev, self.expected)[0])

    def test_no_evidence_is_refused_not_skipped(self):
        with self.assertRaises(SystemExit) as cm:
            live.require(self.xbe, os.path.join(self.tmp.name, "missing.txt"))
        self.assertIn("must not be published", str(cm.exception.code))


if __name__ == "__main__":
    unittest.main()
