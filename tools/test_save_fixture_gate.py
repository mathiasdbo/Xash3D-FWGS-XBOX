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

    def test_a_save_in_the_folder_but_not_in_the_expected_file_is_refused(self):
        # the live gate takes its list from the expected file, so an unlisted save would never be loaded
        write(self.saves, "new.sav", b"x" + KNOWN)
        p = gate.problems(self.saves, self.expected)
        self.assertEqual(len(p), 1)
        self.assertIn("not in the expected file", p[0])

    def test_an_unlisted_save_with_bad_pointers_says_so(self):
        write(self.saves, "new.sav", b"x" + STRANGE + KNOWN)
        self.assertIn("do not all read", gate.problems(self.saves, self.expected)[0])
        write(self.saves, "named.sav", b"JSAV no offsets here, as a name-based save would be")
        self.assertTrue(any("do not all read" in x and "named.sav" in x for x in gate.problems(self.saves, self.expected)))

    def test_a_save_without_pointers_cannot_be_scanned_in(self):
        write(self.saves, "named.sav", b"JSAV no offsets here, as a name-based save would be")
        with self.assertRaises(gate.GateError):
            gate.scan(self.saves, os.path.join(self.tmp.name, "out.txt"))

    def test_git_that_cannot_answer_is_not_a_clean_checkout(self):
        import unittest.mock as mock
        with mock.patch.object(gate.subprocess, "run", side_effect=OSError("no git")):
            with self.assertRaises(gate.GateError):
                gate.dirty_reader_files()
        failed = gate.subprocess.CompletedProcess([], 128, "", "fatal: not a git repository")
        with mock.patch.object(gate.subprocess, "run", return_value=failed):
            with self.assertRaises(gate.GateError):
                gate.dirty_reader_files()
            with self.assertRaises(SystemExit):
                gate.require(self.saves)

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
    def test_the_release_path_needs_live_evidence_and_the_command_line_cannot_skip_it(self):
        import make_payload
        import save_live_gate
        params = inspect.signature(make_payload.main).parameters
        self.assertIs(params["live_gate"].default, save_live_gate.require)
        self.assertNotIn("fixture_gate", params)  # CI has no access to the saves; the evidence is what it can verify
        import subprocess
        r = subprocess.run([sys.executable, make_payload.__file__, "--help"], capture_output=True, text=True)
        self.assertNotIn("live", r.stdout)
        self.assertNotIn("fixture", r.stdout)


if __name__ == "__main__":
    unittest.main()
