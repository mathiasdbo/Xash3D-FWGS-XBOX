import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import stage  # noqa: E402


class BakedMaps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        v = os.path.join(self.tmp.name, "valve")
        os.makedirs(os.path.join(v, "maps"))
        for n in ("maps/c0a0.bsp", "maps/c1a0.bsp", "maps/rocket_frenzy.bsp", "liblist.gam", "halflife.wad"):
            with open(os.path.join(v, *n.split("/")), "wb") as f:
                f.write(b"x")

    def plan(self, baked):
        ok = {"ok": True, "problems": [], "notes": []}
        with mock.patch.object(stage, "inspect", return_value=ok), \
                mock.patch.object(stage.payload_mod, "load", return_value=({}, [])):
            files, _ = stage.plan(self.tmp.name, "hl", None, lambda m: None, baked)
        return sorted(r for r, _, _ in files)

    def test_a_baked_maps_bsp_stays_off_the_disc(self):
        got = self.plan([("valve/c0a0.mpak", b"m", 1), ("valve/c1a0.mpak", b"m", 1)])
        self.assertNotIn("valve/maps/c0a0.bsp", got)
        self.assertNotIn("valve/maps/c1a0.bsp", got)
        self.assertIn("valve/c0a0.mpak", got)
        # a map that did not bake keeps its bsp
        self.assertIn("valve/maps/rocket_frenzy.bsp", got)

    def test_without_a_bake_every_bsp_stays(self):
        got = self.plan(None)
        for m in ("c0a0", "c1a0", "rocket_frenzy"):
            self.assertIn("valve/maps/%s.bsp" % m, got)

    def test_only_top_level_mpaks_count(self):
        got = self.plan([("valve/media/c0a0.mpak", b"m", 1)])
        self.assertIn("valve/maps/c0a0.bsp", got)


if __name__ == "__main__":
    unittest.main()
