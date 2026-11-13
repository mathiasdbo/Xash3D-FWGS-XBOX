import hashlib
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
sys.path.insert(0, os.path.join(REPO, "tools"))
import save_layout_check as slc  # noqa: E402
import test_save_layout_check as small_xbe  # noqa: E402


@unittest.skipUnless(os.path.isdir(EXTRAS), "needs the xash-extras submodule (the payload ships its menu images)")
class PrepatchedPayload(unittest.TestCase):
    def make(self, tmp, *extra, xbe_bytes=None):
        # a small XBE with the server's two tables, and a baseline made from it: the layout check runs on every XBE
        data, gameinit = small_xbe.build_xbe()
        baseline = os.path.join(tmp, "baseline.txt")
        with open(baseline, "w", encoding="utf-8") as f:
            f.write(slc.make_baseline(slc.Xbe(data), "xbe-test", "0" * 64, gameinit))
        self.xbe_bytes = data
        xbe = os.path.join(tmp, "in.xbe")
        with open(xbe, "wb") as f:
            f.write(xbe_bytes if xbe_bytes is not None else data)  # --prepatched ships it as it is
        out = os.path.join(tmp, "payload")
        r = subprocess.run([sys.executable, os.path.join(PACKAGER, "make_payload.py"), "--xbe", xbe, "--out", out,
                            "--prepatched", "--layout-baseline", baseline] + list(extra),
                           capture_output=True, text=True)
        self.last = r
        if xbe_bytes is not None:
            return xbe, out, None
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(os.path.join(out, "manifest.json")) as f:
            build = json.load(f)["titles"]["hl"]["build"]
        return xbe, out, build

    def test_the_xbe_is_shipped_as_it_is_and_the_record_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            xbe, out, build = self.make(tmp)
            with open(xbe, "rb") as a, open(os.path.join(out, "hl", "default.xbe"), "rb") as b:
                self.assertEqual(a.read(), b.read())  # not patched a second time
            self.assertTrue(build["xbe_prepatched"])
            self.assertEqual(build["xbe_source_sha256"], hashlib.sha256(self.xbe_bytes).hexdigest())

    def test_an_xbe_that_moves_the_saves_layout_is_refused_and_nothing_is_written(self):
        ex = {n: 0x100 + 0x40 * i for i, n in enumerate(small_xbe.NAMES)}
        ex["monster_07"] += 16
        moved, _ = small_xbe.build_xbe(exports=ex)
        with tempfile.TemporaryDirectory() as tmp:
            keep = os.path.join(tmp, "payload", "hl")
            os.makedirs(keep)
            with open(os.path.join(keep, "default.xbe"), "wb") as f:
                f.write(b"the last payload")
            _, out, _ = self.make(tmp, xbe_bytes=moved)
            self.assertNotEqual(self.last.returncode, 0)
            self.assertIn("monster_07 +16", self.last.stderr + self.last.stdout)
            with open(os.path.join(out, "hl", "default.xbe"), "rb") as f:
                self.assertEqual(f.read(), b"the last payload")  # the earlier payload is untouched
            self.assertFalse(os.path.exists(os.path.join(out, "manifest.json")))

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
