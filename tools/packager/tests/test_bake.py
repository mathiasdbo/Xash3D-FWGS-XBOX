import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import bake  # noqa: E402


def fake_run(calls, fail_maps=()):
    """Stands in for the baker: writes what the real one would for each mode."""
    def run(baker, args, log_path):
        calls.append(list(args))
        a = dict(zip(args[::2], args[1::2])) if "--bake-menu-art" not in args else {}
        if "--map" in args:
            m = os.path.basename(args[args.index("--map") + 1])[:-4]
            if m in fail_maps:
                return 1
            with open(args[args.index("--output") + 1], "wb") as f:
                f.write(b"MPAK" + m.encode())
            return 0
        if "--bake-menu-art" in args:
            out = args[args.index("--output") + 1]
            os.makedirs(os.path.join(out, "resource", "background"))
            for n in ("BackgroundLayout.txt", "background/bake_0_0.dds"):
                with open(os.path.join(out, "resource", n), "wb") as f:
                    f.write(b"x")
            return 0
        if "--bake-sky-dxt1" in args:
            out = args[args.index("--output") + 1]
            os.makedirs(os.path.join(out, "gfx", "env"))
            with open(os.path.join(out, "gfx", "env", "desertbk.dds"), "wb") as f:
                f.write(b"x")
            return 0
        if "--music" in args:
            out = args[args.index("--music") + 1]
            os.makedirs(os.path.join(out, "media"))
            n = "T.48k.xwav" if "--music-rate" in args else "T.xwav"
            with open(os.path.join(out, "media", n), "wb") as f:
                f.write(b"x")
            return 0
        return 2
    return run


class Bake(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.steam = os.path.join(self.tmp.name, "hl")
        os.makedirs(os.path.join(self.steam, "valve", "maps"))
        for m in ("c0a0", "c1a0", "c1a1"):
            with open(os.path.join(self.steam, "valve", "maps", m + ".bsp"), "wb") as f:
                f.write(b"bsp" + m.encode())
        self.baker = os.path.join(self.tmp.name, bake.BAKER_NAME)
        with open(self.baker, "wb") as f:
            f.write(b"not a real baker")
        self.cache = os.path.join(self.tmp.name, "cache")

    def test_bakes_everything_and_caches(self):
        calls = []
        with mock.patch.object(bake, "_run", fake_run(calls)):
            files = bake.bake(self.steam, "valve", self.baker, self.cache, log=lambda m: None)
            names = sorted(r for r, _, _ in files)
            self.assertEqual(names, ["valve/c0a0.mpak", "valve/c1a0.mpak", "valve/c1a1.mpak",
                                     "valve/gfx/env/desertbk.dds",
                                     "valve/media/T.48k.xwav", "valve/media/T.xwav",
                                     "valve/resource/BackgroundLayout.txt",
                                     "valve/resource/background/bake_0_0.dds"])
            n = len(calls)
            self.assertEqual(n, 3 + 1 + 1 + 2)
            # a second run reads the cache and does not call the baker
            again = bake.bake(self.steam, "valve", self.baker, self.cache, log=lambda m: None)
            self.assertEqual(len(calls), n)
            self.assertEqual(sorted(r for r, _, _ in again), names)

    def test_targets_the_xbox_nv2a_profile(self):
        calls = []
        with mock.patch.object(bake, "_run", fake_run(calls)):
            bake.bake(self.steam, "valve", self.baker, self.cache, log=lambda m: None, jobs=1)
        maps = [c for c in calls if "--map" in c]
        for c in maps:
            self.assertEqual(c[c.index("--target") + 1], "xbox")
            self.assertEqual(c[c.index("--profile") + 1], "nv2a")

    def test_a_changed_map_rebakes(self):
        calls = []
        with mock.patch.object(bake, "_run", fake_run(calls)):
            bake.bake(self.steam, "valve", self.baker, self.cache, log=lambda m: None)
            n = len(calls)
            with open(os.path.join(self.steam, "valve", "maps", "c0a0.bsp"), "ab") as f:
                f.write(b"more")
            bake.bake(self.steam, "valve", self.baker, self.cache, log=lambda m: None)
            self.assertEqual(len(calls), 2 * n)

    def test_one_failed_map_is_reported_not_fatal(self):
        calls, logs = [], []
        with mock.patch.object(bake, "_run", fake_run(calls, fail_maps=("c1a0",))):
            files = bake.bake(self.steam, "valve", self.baker, self.cache, log=logs.append)
        self.assertNotIn("valve/c1a0.mpak", [r for r, _, _ in files])
        self.assertTrue(any("c1a0" in m for m in logs))

    def test_every_map_failing_is_an_error(self):
        with mock.patch.object(bake, "_run", fake_run([], fail_maps=("c0a0", "c1a0", "c1a1"))):
            with self.assertRaises(bake.BakeError):
                bake.bake(self.steam, "valve", self.baker, self.cache, log=lambda m: None)

    def test_missing_baker_says_how_to_build_it(self):
        with mock.patch.object(bake.os.path, "isfile", return_value=False), \
                self.assertRaises(bake.BakeError) as cm:
            bake.find_baker(os.path.join(self.tmp.name, "nope"))
        self.assertIn("build_baker.py", str(cm.exception))


class RenamedCache(unittest.TestCase):
    def test_a_bake_under_the_old_name_is_moved_not_redone(self):
        with tempfile.TemporaryDirectory() as d:
            old = os.path.join(d, "Resonance3D Packager", "bake")
            os.makedirs(old)
            with open(os.path.join(old, "keep"), "w") as f:
                f.write("x")
            with mock.patch.dict(os.environ, {"LOCALAPPDATA": d, "XDG_CACHE_HOME": d}),                     mock.patch.object(bake.os, "name", "nt"):
                root = bake.default_cache_root()
            self.assertTrue(root.endswith(os.path.join("Xash3D-FWGS-XBOX Packager", "bake")))
            self.assertTrue(os.path.exists(os.path.join(root, "keep")))
            self.assertFalse(os.path.exists(old))


class NoConsoleWindows(unittest.TestCase):
    """The windowed packager starts the baker many times; none of them may open a console window."""

    @unittest.skipUnless(os.name == "nt", "console windows are a Windows matter")
    def test_every_baker_start_hides_its_window(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.object(bake.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
                self.assertEqual(bake._run("AssetStudio.exe", ["--map", "x.bsp"], os.path.join(d, "x.log")), 0)
        kw = run.call_args.kwargs
        self.assertTrue(kw["creationflags"] & subprocess.CREATE_NO_WINDOW)
        self.assertTrue(kw["startupinfo"].dwFlags & subprocess.STARTF_USESHOWWINDOW)
        self.assertEqual(kw["startupinfo"].wShowWindow, 0)

    @unittest.skipIf(os.name == "nt", "off Windows there is nothing to hide")
    def test_other_systems_pass_no_extra_options(self):
        self.assertEqual(bake._no_window(), {})


if __name__ == "__main__":
    unittest.main()
