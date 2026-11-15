import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGER = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(PACKAGER))
EXTRAS = os.path.join(REPO, "3rdparty", "extras", "xash-extras", "gfx", "shell")
sys.path.insert(0, PACKAGER)
sys.path.insert(0, os.path.join(REPO, "tools"))
import make_payload  # noqa: E402
import save_layout_check  # noqa: E402
import test_save_layout_check as small_xbe  # noqa: E402


def save_layout_check_advice(data):
    """the notes the release path prints, for a baseline made from the small XBE the tests build"""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "b.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(small_xbe.baseline_text())
        return save_layout_check.advice(data, path, None)


@unittest.skipUnless(os.path.isdir(EXTRAS), "needs the xash-extras submodule (the payload ships its menu images)")
class PrepatchedPayload(unittest.TestCase):
    def make(self, tmp, *extra, xbe_bytes=None):
        """Runs make_payload.main in this process with a baseline made from a small XBE (the layout check runs on every
        XBE; the command line has no way to name a baseline, so the tests hand it in). Returns (xbe, out, build) and
        sets self.refused to the SystemExit message when it was refused."""
        data, _ = small_xbe.build_xbe()
        baseline = os.path.join(tmp, "baseline.txt")
        with open(baseline, "w", encoding="utf-8") as f:
            f.write(small_xbe.baseline_text())
        self.xbe_bytes = data
        xbe = os.path.join(tmp, "in.xbe")
        with open(xbe, "wb") as f:
            f.write(xbe_bytes if xbe_bytes is not None else data)  # --prepatched ships it as it is
        out = os.path.join(tmp, "payload")
        self.refused = None
        try:
            self.printed = io.StringIO()
            with contextlib.redirect_stdout(self.printed):
                make_payload.main(["--xbe", xbe, "--out", out, "--prepatched"] + list(extra), layout_baseline=baseline,
                                  layout_sha256=None, live_gate=None, rules_gate=None)
        except SystemExit as e:
            self.refused = str(e.code)
            return xbe, out, None
        with open(os.path.join(out, "manifest.json")) as f:
            build = json.load(f)["titles"]["hl"]["build"]
        return xbe, out, build

    def test_the_xbe_is_shipped_as_it_is_and_the_record_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            xbe, out, build = self.make(tmp)
            self.assertIsNone(self.refused)
            with open(xbe, "rb") as a, open(os.path.join(out, "hl", "default.xbe"), "rb") as b:
                self.assertEqual(a.read(), b.read())  # not patched a second time
            self.assertTrue(build["xbe_prepatched"])
            self.assertEqual(build["xbe_source_sha256"], hashlib.sha256(self.xbe_bytes).hexdigest())

    def make_noisy(self, tmp, xbe_bytes):
        """like make(), with what main printed"""
        self.make(tmp, xbe_bytes=xbe_bytes)
        return self.printed.getvalue()

    def test_a_moved_layout_is_no_longer_refused_it_is_noted(self):
        # XS.4: saves are written by name and old offsets read through the frozen table; the live evidence decides
        ex = {n: small_xbe.ANCHOR_OFFSET + 0x40 * i for i, n in enumerate(small_xbe.NAMES)}
        ex["monster_07"] += 16
        moved, _ = small_xbe.build_xbe(exports=ex)
        with tempfile.TemporaryDirectory() as tmp:
            _, out, build = self.make(tmp, xbe_bytes=moved)
            self.assertIsNone(self.refused)
            self.assertTrue(os.path.exists(os.path.join(out, "manifest.json")))
        notes = save_layout_check_advice(moved)
        self.assertTrue(any("monster_07 +16" in n for n in notes))

    def test_the_note_is_printed_for_a_changed_function_body(self):
        funcs = small_xbe.default_funcs(0)
        body = bytearray(funcs[small_xbe.FUNC_OFFSETS[2]])
        body[12] ^= 0xFF
        funcs[small_xbe.FUNC_OFFSETS[2]] = bytes(body)
        changed, _ = small_xbe.build_xbe(funcs=funcs)
        with tempfile.TemporaryDirectory() as tmp:
            printed = self.make_noisy(tmp, changed)
        self.assertIn("save layout differs", printed)
        self.assertIn("fn_2", printed)

    def test_a_release_without_live_evidence_is_refused_and_nothing_is_written(self):
        # the live gate is what decides now: nothing is injected, as in a release, and no evidence exists for these bytes
        data, _ = small_xbe.build_xbe()
        with tempfile.TemporaryDirectory() as tmp:
            xbe = os.path.join(tmp, "in.xbe")
            with open(xbe, "wb") as f:
                f.write(data)
            out = os.path.join(tmp, "o")
            with self.assertRaises(SystemExit) as cm:
                with contextlib.redirect_stdout(io.StringIO()):
                    make_payload.main(["--xbe", xbe, "--out", out, "--prepatched"], rules_gate=None)
            self.assertIn("live save gate failed", str(cm.exception.code))
            self.assertFalse(os.path.exists(out))

    def test_an_xbe_with_another_title_id_is_refused_and_nothing_is_written(self):
        # XS.5: saves live in UDATA/<title id>; the layout and live gates are off here, the id check is the real one
        import save_rules_check
        import test_save_rules_check
        data = test_save_rules_check.small_xbe(0x12345678)
        with tempfile.TemporaryDirectory() as tmp:
            xbe = os.path.join(tmp, "in.xbe")
            with open(xbe, "wb") as f:
                f.write(data)
            out = os.path.join(tmp, "o")
            with self.assertRaises(SystemExit) as cm:
                with contextlib.redirect_stdout(io.StringIO()):
                    make_payload.main(["--xbe", xbe, "--out", out, "--prepatched"], layout_sha256=None, live_gate=None)
            self.assertIn("title id", str(cm.exception.code))
            self.assertFalse(os.path.exists(out))

    def test_the_command_line_cannot_name_a_baseline(self):
        # a candidate XBE must not be able to bring its own: the option does not exist
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, os.path.join(PACKAGER, "make_payload.py"), "--xbe",
                                os.path.join(tmp, "x.xbe"), "--out", os.path.join(tmp, "o"),
                                "--layout-baseline", os.path.join(tmp, "b.txt")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn("unrecognized arguments: --layout-baseline", r.stderr)

    def test_the_committed_baseline_and_its_digest_are_the_default(self):
        import inspect
        import save_layout_check
        params = inspect.signature(make_payload.main).parameters
        self.assertEqual(params["layout_baseline"].default, save_layout_check.BASELINE)
        self.assertEqual(os.path.basename(params["layout_baseline"].default), "save-layout-baseline.txt")
        self.assertEqual(params["layout_sha256"].default, save_layout_check.BASELINE_SHA256)
        self.assertEqual(len(save_layout_check.BASELINE_SHA256), 64)

    def test_the_build_commit_can_differ_from_the_checkouts_head(self):
        head = subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if not head:
            self.skipTest("not a git checkout")
        with tempfile.TemporaryDirectory() as tmp:
            _, _, build = self.make(tmp, "--xbe-commit", head)
            self.assertEqual(build["commit"], head)
            self.assertEqual(build["packager_commit"], head)
            _, _, build = self.make(tmp, "--xbe-commit", "0" * 40)
            self.assertEqual(build["commit"], "0" * 40)
            self.assertEqual(build["branch"], "unknown")  # not an ancestor of this checkout
            self.assertEqual(build["packager_commit"], head)


if __name__ == "__main__":
    unittest.main()
