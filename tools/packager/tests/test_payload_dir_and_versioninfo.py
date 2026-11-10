import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import payload, versioninfo  # noqa: E402


class PayloadDir(unittest.TestCase):
    def test_a_frozen_exe_prefers_the_payload_beside_it(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "payload"))
            with mock.patch.object(sys, "frozen", True, create=True), \
                    mock.patch.object(sys, "executable", os.path.join(d, "Packager.exe")), \
                    mock.patch.object(sys, "_MEIPASS", os.path.join(d, "bundle"), create=True):
                self.assertEqual(payload.default_dir(), os.path.join(d, "payload"))

    def test_a_frozen_exe_falls_back_to_the_bundled_payload(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.object(sys, "frozen", True, create=True), \
                    mock.patch.object(sys, "executable", os.path.join(d, "Packager.exe")), \
                    mock.patch.object(sys, "_MEIPASS", os.path.join(d, "bundle"), create=True):
                self.assertEqual(payload.default_dir(), os.path.join(d, "bundle", "payload"))

    def test_a_checkout_uses_the_payload_folder_of_the_packager(self):
        self.assertTrue(payload.default_dir().endswith(os.path.join("packager", "payload")))


class VersionInfo(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(versioninfo.parse("v0.1.1"), (0, 1, 1, 0))
        self.assertEqual(versioninfo.parse("1.2.3.4"), (1, 2, 3, 4))
        self.assertEqual(versioninfo.product_version("v0.1.1"), "0.1.1")

    def test_bad_versions_are_refused(self):
        for bad in ("", "latest", "v1.2.3.4.5", "v1.x", "v70000.0"):
            with self.assertRaises(ValueError):
                versioninfo.parse(bad)

    def test_the_resource_names_the_project_and_the_release(self):
        r = versioninfo.render("v0.1.1", "Xash3D-FWGS-XBOX-Packager.exe")
        self.assertIn("StringStruct('ProductName', 'Xash3D-FWGS-XBOX')", r)
        self.assertIn("StringStruct('ProductVersion', '0.1.1')", r)
        self.assertIn("filevers=(0, 1, 1, 0)", r)
        compile(r, "versioninfo", "eval")  # PyInstaller evaluates it as Python


if __name__ == "__main__":
    unittest.main()
