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


@unittest.skipUnless(os.path.isdir(EXTRAS), "needs the xash-extras submodule (the payload ships its menu images)")
class PrepatchedPayload(unittest.TestCase):
    def make(self, tmp, *extra):
        xbe = os.path.join(tmp, "in.xbe")
        with open(xbe, "wb") as f:
            f.write(b"XBEH" + b"\0" * 4096)  # never parsed: --prepatched ships it as it is
        out = os.path.join(tmp, "payload")
        r = subprocess.run([sys.executable, os.path.join(PACKAGER, "make_payload.py"), "--xbe", xbe, "--out", out,
                            "--prepatched"] + list(extra), capture_output=True, text=True)
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
            self.assertEqual(build["xbe_source_sha256"], hashlib.sha256(b"XBEH" + b"\0" * 4096).hexdigest())

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
