"""XS.4: tools/save_fixture_gate.py. Run: python -m unittest discover -s tools -p "test_save_fixture_gate.py"
The gate compiles the engine's reader with a host C compiler; without one these tests skip."""
import inspect
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools", "packager"))
import save_fixture_gate as gate  # noqa: E402

HAVE_CC = shutil.which(os.environ.get("CC") or "gcc") is not None
KNOWN = b"ofs:4294950224\0"    # BreakTouch@CBreakable
FOLDED = b"ofs:4294756928\0"   # the shared offset, left to the field
STRANGE = b"ofs:1\0"           # no public build wrote it


def write(folder, name, data):
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)


@unittest.skipUnless(HAVE_CC, "no host C compiler")
class Gate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.saves = os.path.join(self.tmp.name, "saves")
        os.mkdir(self.saves)
        self.save = b"HEAD\0" + KNOWN + b"x" + KNOWN + FOLDED + b"tail"
        write(self.saves, "a.sav", self.save)
        self.expected = os.path.join(self.tmp.name, "expected.txt")
        gate.scan(self.saves, self.expected)

    def tearDown(self):
        self.tmp.cleanup()

    def test_scan_records_names_and_counts_and_no_content(self):
        text = open(self.expected, encoding="utf-8").read()
        self.assertIn("BYFIELD=1;BreakTouch@CBreakable=2", text)
        self.assertNotIn("HEAD", text)
        self.assertEqual(gate.problems(self.saves, self.expected), [])

    def test_a_renamed_fixture_is_found_by_its_hash(self):
        os.rename(os.path.join(self.saves, "a.sav"), os.path.join(self.saves, "renamed.bin"))
        self.assertEqual(gate.problems(self.saves, self.expected), [])

    def test_unset_or_missing_folder_is_refused_not_skipped(self):
        self.assertIn("is not set", gate.problems(None, self.expected)[0])
        self.assertIn("is not a folder", gate.problems(os.path.join(self.tmp.name, "nope"), self.expected)[0])

    def test_a_fixture_missing_from_the_folder_fails(self):
        os.remove(os.path.join(self.saves, "a.sav"))
        self.assertIn("is not in", gate.problems(self.saves, self.expected)[0])

    def test_a_pointer_that_resolves_differently_fails(self):
        # what a reader that lost a table row looks like: the same save reads as something else
        with open(self.expected, encoding="utf-8") as f:
            text = f.read().replace("BreakTouch@CBreakable=2", "BreakTouch@CBreakable=1;UNKNOWN=1")
        with open(self.expected, "w", encoding="utf-8") as f:
            f.write(text)
        self.assertTrue(gate.problems(self.saves, self.expected))

    def test_a_new_fixture_with_an_offset_nobody_wrote_fails(self):
        write(self.saves, "new.sav", b"x" + STRANGE + KNOWN)
        p = gate.problems(self.saves, self.expected)
        self.assertEqual(len(p), 1)
        self.assertIn("do not read", p[0])

    def test_a_new_fixture_that_reads_is_accepted(self):
        write(self.saves, "new.sav", b"x" + KNOWN)
        self.assertEqual(gate.problems(self.saves, self.expected), [])

    def test_require_exits_with_the_reason(self):
        with self.assertRaises(SystemExit) as cm:
            gate.require(os.path.join(self.tmp.name, "nope"), self.expected)
        self.assertIn("must not be published", str(cm.exception.code))
        gate.require(self.saves, self.expected)

    def test_the_committed_expected_file_parses_and_names_every_fixture_as_resolved(self):
        rows = gate.parse_expected(open(gate.EXPECTED, encoding="utf-8").read())
        self.assertGreaterEqual(len(rows), 24)
        for name, counts in rows.values():
            self.assertFalse({"UNKNOWN", "BAD"} & set(counts), name)
            self.assertTrue(counts, name)


class Release(unittest.TestCase):
    def test_the_release_path_runs_the_gate_and_the_command_line_cannot_skip_it(self):
        import make_payload
        params = inspect.signature(make_payload.main).parameters
        self.assertIs(params["fixture_gate"].default, gate.require)
        import subprocess
        r = subprocess.run([sys.executable, make_payload.__file__, "--help"], capture_output=True, text=True)
        self.assertNotIn("fixture", r.stdout)


if __name__ == "__main__":
    unittest.main()
