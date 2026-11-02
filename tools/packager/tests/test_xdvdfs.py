import os, subprocess, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import xdvdfs

EXTRACT = os.path.join("G:/Proyectos/Ferrum56/resonance3d/deps/nxdk",
                       "tools/extract-xiso/build/extract-xiso.exe") if os.path.exists("G:/Proyectos/Ferrum56/r3d-visual/.sweep/nxdk-dir") else ""


def sample(n=300):
    files = [("default.xbe", b"XBEH" + b"\x01" * 5000, 5004)]
    for i in range(n):
        data = bytes([i % 251]) * (i * 37 % 5000 + 1)
        files.append(("valve/maps/map%03d.bsp" % i, data, len(data)))
    files.append(("valve/Liblist.gam", b"game", 4))
    files.append(("valve/empty.txt", b"", 0))
    return files


class T(unittest.TestCase):
    def test_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            iso = os.path.join(d, "t.iso")
            files = sample()
            xdvdfs.write_xiso(files, iso)
            got = xdvdfs.read_xiso(iso)
            self.assertEqual(os.path.getsize(iso) % 2048, 0)
            with open(iso, "rb") as f:
                for rel, data, size in files:
                    sector, sz = got[rel]
                    self.assertEqual(sz, size, rel)
                    if size:
                        f.seek(sector * 2048)
                        self.assertEqual(f.read(size), data, rel)

    def test_duplicate_case(self):
        with self.assertRaises(xdvdfs.XisoError):
            xdvdfs.build_tree([("a/B", b"x", 1), ("a/b", b"y", 1)])

    @unittest.skipUnless(EXTRACT and os.path.exists(EXTRACT), "extract-xiso not available")
    def test_extract_xiso_agrees(self):
        with tempfile.TemporaryDirectory() as d:
            iso = os.path.join(d, "t.iso")
            files = sample()
            xdvdfs.write_xiso(files, iso)
            out = subprocess.run([EXTRACT, "-l", iso], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            listed = out.stdout
            for rel, _, _ in files[:5] + files[-3:]:
                self.assertIn(rel.split("/")[-1], listed)
            ex = os.path.join(d, "x")
            os.mkdir(ex)
            x = subprocess.run([EXTRACT, "-d", ex, "-x", iso], capture_output=True, text=True)
            self.assertEqual(x.returncode, 0, x.stdout + x.stderr)
            for rel, data, size in files:
                p = os.path.join(ex, *rel.split("/"))
                self.assertTrue(os.path.exists(p) or rel.endswith("empty.txt"), rel)
                if size:
                    self.assertEqual(open(p, "rb").read(), data, rel)


if __name__ == "__main__":
    unittest.main()
